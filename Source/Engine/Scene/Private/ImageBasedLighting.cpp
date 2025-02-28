#include "Engine/Scene/ImageBasedLighting.hpp"

#include "Engine/Filesystem/Filepath.hpp"
#include "Engine/Render/RenderContext.hpp"
#include "Engine/Render/Vulkan/PipelineHelpers.hpp"
#include "Engine/Render/Vulkan/DescriptorHelpers.hpp"
#include "Engine/Render/Vulkan/VulkanContext.hpp"
#include "Engine/Render/Vulkan/ComputePipeline.hpp"
#include "Engine/Render/Vulkan/VulkanConfig.hpp"

#include "Utils/TimeHelpers.hpp"

namespace Details
{
    static constexpr glm::uvec2 kWorkGroupSize(8, 8);

    static constexpr vk::Extent2D kSpecularBRDFExtent(256, 256);

    static constexpr vk::Extent2D kMaxIrradianceExtent(128, 128);

    static constexpr vk::Extent2D kMaxReflectionExtent(512, 512);

    static const Filepath kSpecularBRDFShaderPath("~/Shaders/Compute/ImageBasedLighting/SpecularBRDF.comp");
    static const Filepath kIrradianceShaderPath("~/Shaders/Compute/ImageBasedLighting/Irradiance.comp");
    static const Filepath kReflectionShaderPath("~/Shaders/Compute/ImageBasedLighting/Reflection.comp");

    static ImageBasedLighting::Samplers CreateSamplers()
    {
        const SamplerDescription irradianceDescription{
            vk::Filter::eLinear,
            vk::Filter::eLinear,
            vk::SamplerMipmapMode::eNearest,
            vk::SamplerAddressMode::eRepeat,
            std::nullopt,
            0.0f, 0.0f,
            false
        };

        const SamplerDescription reflectionDescription{
            vk::Filter::eLinear,
            vk::Filter::eLinear,
            vk::SamplerMipmapMode::eLinear,
            vk::SamplerAddressMode::eRepeat,
            std::nullopt,
            0.0f, std::numeric_limits<float>::max(),
            false
        };

        const SamplerDescription specularBRDFDescription{
            vk::Filter::eNearest,
            vk::Filter::eNearest,
            vk::SamplerMipmapMode::eNearest,
            vk::SamplerAddressMode::eClampToEdge,
            std::nullopt,
            0.0f, 0.0f,
            false
        };

        const ImageBasedLighting::Samplers samplers{
            VulkanContext::textureManager->CreateSampler(irradianceDescription),
            VulkanContext::textureManager->CreateSampler(reflectionDescription),
            VulkanContext::textureManager->CreateSampler(specularBRDFDescription),
        };

        return samplers;
    }

    static vk::DescriptorSetLayout CreateEnvironmentLayout()
    {
        const DescriptorPool& descriptorPool = *VulkanContext::descriptorPool;

        const DescriptorDescription cubemapDescriptorDescription{
            1, vk::DescriptorType::eCombinedImageSampler,
            vk::ShaderStageFlagBits::eCompute,
            vk::DescriptorBindingFlags()
        };

        return descriptorPool.CreateDescriptorSetLayout({ cubemapDescriptorDescription });
    }

    static vk::DescriptorSetLayout CreateTargetLayout()
    {
        const DescriptorPool& descriptorPool = *VulkanContext::descriptorPool;

        const DescriptorDescription targetDescriptorDescription{
            1, vk::DescriptorType::eStorageImage,
            vk::ShaderStageFlagBits::eCompute,
            vk::DescriptorBindingFlags()
        };

        return descriptorPool.CreateDescriptorSetLayout({ targetDescriptorDescription });
    }

    static std::unique_ptr<ComputePipeline> CreateIrradiancePipeline(
            const std::vector<vk::DescriptorSetLayout>& layouts)
    {
        constexpr std::tuple specializationValues = std::make_tuple(kWorkGroupSize.x, kWorkGroupSize.y);

        const ShaderModule shaderModule = VulkanContext::shaderManager->CreateShaderModule(
                vk::ShaderStageFlagBits::eCompute, kIrradianceShaderPath, {}, specializationValues);

        const vk::PushConstantRange pushConstantRange(
                vk::ShaderStageFlagBits::eCompute, 0, sizeof(uint32_t));

        const ComputePipeline::Description pipelineDescription{
            shaderModule, layouts, { pushConstantRange }
        };

        std::unique_ptr<ComputePipeline> pipeline = ComputePipeline::Create(pipelineDescription);

        VulkanContext::shaderManager->DestroyShaderModule(shaderModule);

        return pipeline;
    }

    static std::unique_ptr<ComputePipeline> CreateReflectionPipeline(
            const std::vector<vk::DescriptorSetLayout>& layouts)
    {
        constexpr std::tuple specializationValues = std::make_tuple(
                kWorkGroupSize.x, kWorkGroupSize.y, Config::kMaxEnvironmentLuminance);

        const ShaderModule shaderModule = VulkanContext::shaderManager->CreateShaderModule(
                vk::ShaderStageFlagBits::eCompute, kReflectionShaderPath, {}, specializationValues);

        const vk::PushConstantRange pushConstantRange(
                vk::ShaderStageFlagBits::eCompute, 0, sizeof(float) + sizeof(uint32_t));

        const ComputePipeline::Description pipelineDescription{
            shaderModule, layouts, { pushConstantRange }
        };

        std::unique_ptr<ComputePipeline> pipeline = ComputePipeline::Create(pipelineDescription);

        VulkanContext::shaderManager->DestroyShaderModule(shaderModule);

        return pipeline;
    }

    static Texture CreateSpecularBRDF(vk::DescriptorSetLayout targetLayout)
    {
        constexpr std::tuple specializationValues = std::make_tuple(kWorkGroupSize.x, kWorkGroupSize.y);

        const ShaderModule shaderModule = VulkanContext::shaderManager->CreateShaderModule(
                vk::ShaderStageFlagBits::eCompute, kSpecularBRDFShaderPath, {}, specializationValues);

        const ComputePipeline::Description pipelineDescription{
            shaderModule, { targetLayout }, {}
        };

        std::unique_ptr<ComputePipeline> pipeline = ComputePipeline::Create(pipelineDescription);

        VulkanContext::shaderManager->DestroyShaderModule(shaderModule);

        const vk::ImageUsageFlags imageUsage = vk::ImageUsageFlagBits::eTransferDst
                | vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;

        const ImageDescription imageDescription{
            ImageType::e2D, vk::Format::eR16G16Sfloat,
            VulkanHelpers::GetExtent3D(kSpecularBRDFExtent),
            1, 1, vk::SampleCountFlagBits::e1,
            vk::ImageTiling::eOptimal, imageUsage,
            vk::MemoryPropertyFlagBits::eDeviceLocal
        };

        const vk::Image image = VulkanContext::imageManager->CreateImage(
                imageDescription, ImageCreateFlags::kNone);

        const vk::ImageView view = VulkanContext::imageManager->CreateView(
                image, vk::ImageViewType::e2D, ImageHelpers::kFlatColor);

        DescriptorPool& descriptorPool = *VulkanContext::descriptorPool;

        const vk::DescriptorSet descriptorSet = descriptorPool.AllocateDescriptorSets({ targetLayout }).front();

        descriptorPool.UpdateDescriptorSet({ descriptorSet }, { DescriptorHelpers::GetStorageData(view) }, 0);

        VulkanContext::device->ExecuteOneTimeCommands([&](vk::CommandBuffer commandBuffer)
            {
                {
                    const ImageLayoutTransition layoutTransition{
                        vk::ImageLayout::eUndefined,
                        vk::ImageLayout::eGeneral,
                        PipelineBarrier{
                            SyncScope::kWaitForNone,
                            SyncScope::kComputeShaderWrite
                        }
                    };

                    ImageHelpers::TransitImageLayout(commandBuffer, image,
                            ImageHelpers::kFlatColor, layoutTransition);
                }

                const glm::uvec3 groupCount = PipelineHelpers::CalculateWorkGroupCount(
                        kSpecularBRDFExtent, Details::kWorkGroupSize);

                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Get());

                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                        pipeline->GetLayout(), 0, { descriptorSet }, {});

                commandBuffer.dispatch(groupCount.x, groupCount.y, groupCount.z);

                {
                    const ImageLayoutTransition layoutTransition{
                        vk::ImageLayout::eGeneral,
                        vk::ImageLayout::eShaderReadOnlyOptimal,
                        PipelineBarrier{
                            SyncScope::kComputeShaderWrite,
                            SyncScope::kBlockNone
                        }
                    };

                    ImageHelpers::TransitImageLayout(commandBuffer, image,
                            ImageHelpers::kFlatColor, layoutTransition);
                }
            });

        descriptorPool.FreeDescriptorSets({ descriptorSet });

        VulkanHelpers::SetObjectName(VulkanContext::device->Get(), image, "SpecularBRDF");

        return Texture{ image, view };
    }

    static const vk::Extent2D& GetIrradianceExtent(const vk::Extent2D& cubemapExtent)
    {
        if (cubemapExtent.width <= kMaxIrradianceExtent.width)
        {
            return cubemapExtent;
        }

        return kMaxIrradianceExtent;
    }

    static const vk::Extent2D& GetReflectionExtent(const vk::Extent2D& cubemapExtent)
    {
        if (cubemapExtent.width <= kMaxReflectionExtent.width)
        {
            return cubemapExtent;
        }

        return kMaxReflectionExtent;
    }

    static vk::Image CreateIrradianceImage(vk::Format format, const vk::Extent2D& extent)
    {
        constexpr vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;

        const ImageDescription imageDescription{
            ImageType::eCube, format,
            VulkanHelpers::GetExtent3D(extent),
            1, ImageHelpers::kCubeFaceCount,
            vk::SampleCountFlagBits::e1,
            vk::ImageTiling::eOptimal, usage,
            vk::MemoryPropertyFlagBits::eDeviceLocal
        };

        return VulkanContext::imageManager->CreateImage(imageDescription, ImageCreateFlags::kNone);
    }

    static vk::Image CreateReflectionImage(vk::Format format, const vk::Extent2D& extent)
    {
        constexpr vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;

        const ImageDescription imageDescription{
            ImageType::eCube, format,
            VulkanHelpers::GetExtent3D(extent),
            ImageHelpers::CalculateMipLevelCount(extent),
            ImageHelpers::kCubeFaceCount,
            vk::SampleCountFlagBits::e1,
            vk::ImageTiling::eOptimal, usage,
            vk::MemoryPropertyFlagBits::eDeviceLocal
        };

        return VulkanContext::imageManager->CreateImage(imageDescription, ImageCreateFlags::kNone);
    }

    static vk::DescriptorSet AllocateEnvironmentDescriptorSet(
            vk::DescriptorSetLayout layout, vk::ImageView cubemapView)
    {
        const DescriptorPool& descriptorPool = *VulkanContext::descriptorPool;

        const vk::DescriptorSet descriptorSet = descriptorPool.AllocateDescriptorSets({ layout }).front();

        const DescriptorData descriptorData =
                DescriptorHelpers::GetData(RenderContext::defaultSampler, cubemapView);

        descriptorPool.UpdateDescriptorSet(descriptorSet, { descriptorData }, 0);

        return descriptorSet;
    }

    static std::vector<vk::DescriptorSet> AllocateCubeFacesDescriptorSets(
            vk::DescriptorSetLayout layout, const ImageHelpers::CubeFacesViews& cubeFacesViews)
    {
        std::vector<vk::DescriptorSet> cubeFacesDescriptorSets
                = VulkanContext::descriptorPool->AllocateDescriptorSets(Repeat(layout, cubeFacesViews.size()));

        for (size_t i = 0; i < cubeFacesViews.size(); ++i)
        {
            const DescriptorData descriptorData = DescriptorHelpers::GetStorageData(cubeFacesViews[i]);

            VulkanContext::descriptorPool->UpdateDescriptorSet(cubeFacesDescriptorSets[i], { descriptorData }, 0);
        }

        return cubeFacesDescriptorSets;
    }
}

ImageBasedLighting::ImageBasedLighting()
{
    cubemapLayout = Details::CreateEnvironmentLayout();
    targetLayout = Details::CreateTargetLayout();

    irradiancePipeline = Details::CreateIrradiancePipeline({ cubemapLayout, targetLayout });
    reflectionPipeline = Details::CreateReflectionPipeline({ cubemapLayout, targetLayout });

    specularBRDF = Details::CreateSpecularBRDF(targetLayout);

    samplers = Details::CreateSamplers();
}

ImageBasedLighting::~ImageBasedLighting()
{
    VulkanContext::descriptorPool->DestroyDescriptorSetLayout(cubemapLayout);
    VulkanContext::descriptorPool->DestroyDescriptorSetLayout(targetLayout);

    VulkanContext::textureManager->DestroyTexture(specularBRDF);

    VulkanContext::textureManager->DestroySampler(samplers.specularBRDF);
    VulkanContext::textureManager->DestroySampler(samplers.irradiance);
    VulkanContext::textureManager->DestroySampler(samplers.reflection);
}

Texture ImageBasedLighting::GenerateIrradianceTexture(const Texture& cubemapTexture) const
{
    EASY_FUNCTION()

    const ImageDescription& cubemapDescription
            = VulkanContext::imageManager->GetImageDescription(cubemapTexture.image);

    const vk::Extent2D cubemapExtent = VulkanHelpers::GetExtent2D(cubemapDescription.extent);
    const vk::Extent2D irradianceExtent = Details::GetIrradianceExtent(cubemapExtent);

    const vk::Image irradianceImage = Details::CreateIrradianceImage(cubemapDescription.format, irradianceExtent);
    const ImageHelpers::CubeFacesViews irradianceFacesViews = ImageHelpers::CreateCubeFacesViews(irradianceImage, 0);

    const vk::DescriptorSet cubemapDescriptorSet
            = Details::AllocateEnvironmentDescriptorSet(cubemapLayout, cubemapTexture.view);
    const std::vector<vk::DescriptorSet> irradianceFacesDescriptorSets
            = Details::AllocateCubeFacesDescriptorSets(targetLayout, irradianceFacesViews);

    for (uint32_t faceIndex = 0; faceIndex < ImageHelpers::kCubeFaceCount; ++faceIndex)
    {
        VulkanContext::device->ExecuteOneTimeCommands([&](vk::CommandBuffer commandBuffer)
            {
                if (faceIndex == 0)
                {
                    const ImageLayoutTransition layoutTransition{
                        vk::ImageLayout::eUndefined,
                        vk::ImageLayout::eGeneral,
                        PipelineBarrier{
                            SyncScope::kWaitForNone,
                            SyncScope::kComputeShaderWrite
                        }
                    };

                    ImageHelpers::TransitImageLayout(commandBuffer, irradianceImage,
                            ImageHelpers::kCubeColor, layoutTransition);
                }

                const std::vector<vk::DescriptorSet> descriptorSets{
                    cubemapDescriptorSet, irradianceFacesDescriptorSets[faceIndex]
                };

                const glm::uvec3 groupCount = PipelineHelpers::CalculateWorkGroupCount(
                        irradianceExtent, Details::kWorkGroupSize);

                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, irradiancePipeline->Get());

                commandBuffer.pushConstants<uint32_t>(irradiancePipeline->GetLayout(),
                        vk::ShaderStageFlagBits::eCompute, 0, { faceIndex });

                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                        irradiancePipeline->GetLayout(), 0, { descriptorSets }, {});

                commandBuffer.dispatch(groupCount.x, groupCount.y, groupCount.z);

                if (faceIndex == ImageHelpers::kCubeFaceCount - 1)
                {
                    const ImageLayoutTransition layoutTransition{
                        vk::ImageLayout::eGeneral,
                        vk::ImageLayout::eShaderReadOnlyOptimal,
                        PipelineBarrier{
                            SyncScope::kComputeShaderWrite,
                            SyncScope::kBlockNone
                        }
                    };

                    ImageHelpers::TransitImageLayout(commandBuffer, irradianceImage,
                            ImageHelpers::kCubeColor, layoutTransition);
                }
            });
    }

    VulkanContext::descriptorPool->FreeDescriptorSets({ cubemapDescriptorSet });
    VulkanContext::descriptorPool->FreeDescriptorSets(irradianceFacesDescriptorSets);

    for (const auto& view : irradianceFacesViews)
    {
        VulkanContext::imageManager->DestroyImageView(irradianceImage, view);
    }

    const vk::ImageView irradianceView = VulkanContext::imageManager->CreateView(
            irradianceImage, vk::ImageViewType::eCube, ImageHelpers::kCubeColor);

    VulkanHelpers::SetObjectName(VulkanContext::device->Get(), irradianceImage, "IrradianceMap");

    return Texture{ irradianceImage, irradianceView };
}

Texture ImageBasedLighting::GenerateReflectionTexture(const Texture& cubemapTexture) const
{
    EASY_FUNCTION()

    const ImageDescription& cubemapDescription
            = VulkanContext::imageManager->GetImageDescription(cubemapTexture.image);

    const vk::Extent2D cubemapExtent = VulkanHelpers::GetExtent2D(cubemapDescription.extent);
    const vk::Extent2D reflectionExtent = Details::GetReflectionExtent(cubemapExtent);

    const vk::Image reflectionImage = Details::CreateReflectionImage(cubemapDescription.format, reflectionExtent);

    const uint32_t reflectionMipLevelCount = ImageHelpers::CalculateMipLevelCount(reflectionExtent);
    std::vector<ImageHelpers::CubeFacesViews> reflectionMipLevelsFacesViews(reflectionMipLevelCount);
    for (uint32_t i = 0; i < reflectionMipLevelsFacesViews.size(); ++i)
    {
        reflectionMipLevelsFacesViews[i] = ImageHelpers::CreateCubeFacesViews(reflectionImage, i);
    }

    const vk::DescriptorSet cubemapDescriptorSet
            = Details::AllocateEnvironmentDescriptorSet(cubemapLayout, cubemapTexture.view);

    std::vector<std::vector<vk::DescriptorSet>> reflectionMipLevelsFacesDescriptorSets(reflectionMipLevelCount);
    for (uint32_t i = 0; i < reflectionMipLevelsFacesViews.size(); ++i)
    {
        reflectionMipLevelsFacesDescriptorSets[i]
                = Details::AllocateCubeFacesDescriptorSets(targetLayout, reflectionMipLevelsFacesViews[i]);
    }

    const vk::ImageSubresourceRange reflectionSubresourceRange(
            vk::ImageAspectFlagBits::eColor,
            0, reflectionMipLevelCount,
            0, ImageHelpers::kCubeFaceCount);

    for (uint32_t faceIndex = 0; faceIndex < ImageHelpers::kCubeFaceCount; ++faceIndex)
    {
        VulkanContext::device->ExecuteOneTimeCommands([&](vk::CommandBuffer commandBuffer)
            {
                if (faceIndex == 0)
                {
                    const ImageLayoutTransition layoutTransition{
                        vk::ImageLayout::eUndefined,
                        vk::ImageLayout::eGeneral,
                        PipelineBarrier{
                            SyncScope::kWaitForNone,
                            SyncScope::kComputeShaderWrite
                        }
                    };

                    ImageHelpers::TransitImageLayout(commandBuffer, reflectionImage,
                            reflectionSubresourceRange, layoutTransition);
                }

                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, reflectionPipeline->Get());

                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                        reflectionPipeline->GetLayout(), 0, { cubemapDescriptorSet }, {});

                for (uint32_t mipLevel = 0; mipLevel < reflectionMipLevelCount; ++mipLevel)
                {
                    const vk::Extent2D mipLevelExtent
                            = ImageHelpers::CalculateMipLevelExtent(reflectionExtent, mipLevel);

                    const glm::uvec3 groupCount
                            = PipelineHelpers::CalculateWorkGroupCount(mipLevelExtent, Details::kWorkGroupSize);

                    const vk::DescriptorSet reflectionFaceDescriptorSet
                            = reflectionMipLevelsFacesDescriptorSets[mipLevel][faceIndex];

                    const float maxMipLevel = static_cast<float>(reflectionMipLevelCount - 1);
                    const float roughness = static_cast<float>(mipLevel) / maxMipLevel;

                    const Bytes pushConstantsBytes = GetBytes(roughness, faceIndex);

                    commandBuffer.pushConstants<uint8_t>(reflectionPipeline->GetLayout(),
                            vk::ShaderStageFlagBits::eCompute, 0, pushConstantsBytes);

                    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                            reflectionPipeline->GetLayout(), 1, { reflectionFaceDescriptorSet }, {});

                    commandBuffer.dispatch(groupCount.x, groupCount.y, groupCount.z);
                }

                if (faceIndex == ImageHelpers::kCubeFaceCount - 1)
                {
                    const ImageLayoutTransition layoutTransition{
                        vk::ImageLayout::eGeneral,
                        vk::ImageLayout::eShaderReadOnlyOptimal,
                        PipelineBarrier{
                            SyncScope::kComputeShaderWrite,
                            SyncScope::kBlockNone
                        }
                    };

                    ImageHelpers::TransitImageLayout(commandBuffer, reflectionImage,
                            reflectionSubresourceRange, layoutTransition);
                }
            });
    }

    VulkanContext::descriptorPool->FreeDescriptorSets({ cubemapDescriptorSet });
    for (const auto& reflectionFacesDescriptorSets : reflectionMipLevelsFacesDescriptorSets)
    {
        VulkanContext::descriptorPool->FreeDescriptorSets(reflectionFacesDescriptorSets);
    }

    for (const auto& reflectionFacesViews : reflectionMipLevelsFacesViews)
    {
        for (const auto& view : reflectionFacesViews)
        {
            VulkanContext::imageManager->DestroyImageView(reflectionImage, view);
        }
    }

    const vk::ImageView reflectionView = VulkanContext::imageManager->CreateView(
            reflectionImage, vk::ImageViewType::eCube, reflectionSubresourceRange);

    VulkanHelpers::SetObjectName(VulkanContext::device->Get(), reflectionImage, "ReflectionMap");

    return Texture{ reflectionImage, reflectionView };
}
