#include "Engine/Render/Vulkan/RayTracing/AccelerationStructureManager.hpp"

#include "Engine/Render/Vulkan/VulkanContext.hpp"

namespace Details
{
    constexpr vk::AabbPositionsKHR kUnitBBox(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f);

    using AccelerationStructureEntry = std::pair<vk::AccelerationStructureKHR, vk::Buffer>;

    static vk::AccelerationStructureBuildSizesInfoKHR GetBuildSizesInfo(vk::AccelerationStructureTypeKHR type,
            const vk::AccelerationStructureGeometryKHR& geometry, uint32_t primitiveCount)
    {
        const vk::AccelerationStructureBuildGeometryInfoKHR buildInfo(
                type, vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
                vk::BuildAccelerationStructureModeKHR::eBuild,
                vk::AccelerationStructureKHR(), vk::AccelerationStructureKHR(),
                1, &geometry, nullptr, vk::DeviceOrHostAddressKHR(), nullptr);

        return VulkanContext::device->Get().getAccelerationStructureBuildSizesKHR(
                vk::AccelerationStructureBuildTypeKHR::eDevice, buildInfo,
                { primitiveCount });
    }

    static vk::Buffer CreateAccelerationStructureBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage)
    {
        const BufferDescription bufferDescription{
            size, usage | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            vk::MemoryPropertyFlagBits::eDeviceLocal
        };

        BufferCreateFlags createFlags;

        if (usage & vk::BufferUsageFlagBits::eStorageBuffer)
        {
            createFlags |= BufferCreateFlagBits::eScratchBuffer;
        }

        const vk::Buffer buffer = VulkanContext::bufferManager->CreateBuffer(bufferDescription, createFlags);

        return buffer;
    }

    static vk::TransformMatrixKHR GetInstanceTransformMatrix(const glm::mat4 transform)
    {
        const glm::mat4 transposedTransform = glm::transpose(transform);

        std::array<std::array<float, 4>, 3> transposedData;

        std::memcpy(&transposedData, &transposedTransform, sizeof(vk::TransformMatrixKHR));

        return vk::TransformMatrixKHR(transposedData);
    }

    static vk::Buffer CreateInstanceBuffer(const std::vector<TlasInstanceData>& instances)
    {
        std::vector<vk::AccelerationStructureInstanceKHR> vkInstances;
        vkInstances.reserve(instances.size());

        for (const auto& instance : instances)
        {
            const vk::AccelerationStructureInstanceKHR vkInstance(
                    GetInstanceTransformMatrix(instance.transform),
                    instance.customIndex, instance.mask,
                    instance.sbtRecordOffset, instance.flags,
                    VulkanContext::device->GetAddress(instance.blas));

            vkInstances.push_back(vkInstance);
        }

        const vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eShaderDeviceAddress
                | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;

        const vk::Buffer buffer = BufferHelpers::CreateBufferWithData(usage, ByteView(vkInstances));

        return buffer;
    }

    static AccelerationStructureEntry GenerateAccelerationStructure(vk::AccelerationStructureTypeKHR type,
            const vk::AccelerationStructureGeometryKHR& geometry, uint32_t primitiveCount)
    {
        const vk::AccelerationStructureBuildSizesInfoKHR buildSizesInfo
                = Details::GetBuildSizesInfo(type, geometry, primitiveCount);

        const vk::Buffer storageBuffer = Details::CreateAccelerationStructureBuffer(
                buildSizesInfo.accelerationStructureSize, vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR);

        const vk::Buffer buildScratchBuffer = Details::CreateAccelerationStructureBuffer(
                buildSizesInfo.buildScratchSize, vk::BufferUsageFlagBits::eStorageBuffer);

        const vk::AccelerationStructureCreateInfoKHR createInfo({}, storageBuffer, 0,
                buildSizesInfo.accelerationStructureSize, type, vk::DeviceAddress());

        const auto [result, accelerationStructure]
                = VulkanContext::device->Get().createAccelerationStructureKHR(createInfo);

        Assert(result == vk::Result::eSuccess);

        const vk::AccelerationStructureBuildGeometryInfoKHR buildInfo(
                type, vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
                vk::BuildAccelerationStructureModeKHR::eBuild,
                nullptr, accelerationStructure, 1, &geometry, nullptr,
                VulkanContext::device->GetAddress(buildScratchBuffer));

        const vk::AccelerationStructureBuildRangeInfoKHR offsetInfo(primitiveCount, 0, 0, 0);
        const vk::AccelerationStructureBuildRangeInfoKHR* pOffsetInfo = &offsetInfo;

        VulkanContext::device->ExecuteOneTimeCommands([&](vk::CommandBuffer commandBuffer)
            {
                commandBuffer.buildAccelerationStructuresKHR({ buildInfo }, { pOffsetInfo });
            });

        VulkanContext::bufferManager->DestroyBuffer(buildScratchBuffer);

        return std::make_pair(accelerationStructure, storageBuffer);
    }
}

vk::AccelerationStructureKHR AccelerationStructureManager::GenerateUnitBBoxBlas()
{
    const vk::AccelerationStructureTypeKHR type = vk::AccelerationStructureTypeKHR::eBottomLevel;

    const vk::BufferUsageFlags bufferUsage = vk::BufferUsageFlagBits::eShaderDeviceAddress
            | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;

    const vk::Buffer bboxBuffer
            = BufferHelpers::CreateBufferWithData(bufferUsage, ByteView(Details::kUnitBBox));

    const vk::AccelerationStructureGeometryAabbsDataKHR bboxData(
            VulkanContext::device->GetAddress(bboxBuffer), sizeof(vk::AabbPositionsKHR));

    const vk::AccelerationStructureGeometryDataKHR geometryData(bboxData);

    const vk::AccelerationStructureGeometryKHR geometry(
            vk::GeometryTypeKHR::eAabbs, geometryData,
            vk::GeometryFlagsKHR());

    const auto [blas, storageBuffer] = Details::GenerateAccelerationStructure(type, geometry, 1);

    VulkanContext::bufferManager->DestroyBuffer(bboxBuffer);

    accelerationStructures.emplace(blas, storageBuffer);

    return blas;
}

vk::AccelerationStructureKHR AccelerationStructureManager::GenerateBlas(const BlasGeometryData& geometryData)
{
    constexpr vk::AccelerationStructureTypeKHR type = vk::AccelerationStructureTypeKHR::eBottomLevel;

    constexpr vk::BufferUsageFlags bufferUsage = vk::BufferUsageFlagBits::eShaderDeviceAddressEXT
            | vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;

    const vk::Buffer vertexBuffer = BufferHelpers::CreateBufferWithData(bufferUsage, ByteView(geometryData.vertices));
    const vk::Buffer indexBuffer = BufferHelpers::CreateBufferWithData(bufferUsage, ByteView(geometryData.indices));

    const vk::AccelerationStructureGeometryTrianglesDataKHR trianglesData(
            geometryData.vertexFormat, VulkanContext::device->GetAddress(vertexBuffer),
            geometryData.vertexStride, geometryData.vertexCount - 1,
            geometryData.indexType, VulkanContext::device->GetAddress(indexBuffer), nullptr);

    const vk::AccelerationStructureGeometryKHR geometry(
            vk::GeometryTypeKHR::eTriangles, trianglesData,
            vk::GeometryFlagsKHR());

    const uint32_t primitiveCount = geometryData.indexCount / 3;

    const auto [blas, storageBuffer] = Details::GenerateAccelerationStructure(type, geometry, primitiveCount);

    accelerationStructures.emplace(blas, storageBuffer);

    VulkanContext::bufferManager->DestroyBuffer(vertexBuffer);
    VulkanContext::bufferManager->DestroyBuffer(indexBuffer);

    return blas;
}

vk::AccelerationStructureKHR AccelerationStructureManager::GenerateTlas(const std::vector<TlasInstanceData>& instances)
{
    constexpr vk::AccelerationStructureTypeKHR type = vk::AccelerationStructureTypeKHR::eTopLevel;

    const vk::Buffer instanceBuffer = Details::CreateInstanceBuffer(instances);

    const vk::AccelerationStructureGeometryInstancesDataKHR instancesData(
            false, VulkanContext::device->GetAddress(instanceBuffer));

    const vk::AccelerationStructureGeometryDataKHR geometryData(instancesData);

    const vk::AccelerationStructureGeometryKHR geometry(
            vk::GeometryTypeKHR::eInstances, geometryData,
            vk::GeometryFlagBitsKHR::eOpaque);

    const uint32_t instanceCount = static_cast<uint32_t>(instances.size());

    const auto [tlas, storageBuffer] = Details::GenerateAccelerationStructure(type, geometry, instanceCount);

    VulkanContext::bufferManager->DestroyBuffer(instanceBuffer);

    accelerationStructures.emplace(tlas, storageBuffer);

    return tlas;
}

void AccelerationStructureManager::DestroyAccelerationStructure(vk::AccelerationStructureKHR accelerationStructure)
{
    const auto it = accelerationStructures.find(accelerationStructure);
    Assert(it != accelerationStructures.end());

    VulkanContext::device->Get().destroyAccelerationStructureKHR(accelerationStructure);
    VulkanContext::bufferManager->DestroyBuffer(it->second);

    accelerationStructures.erase(it);
}
