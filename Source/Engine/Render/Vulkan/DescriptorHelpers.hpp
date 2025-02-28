#pragma once

struct SampledTexture;

struct DescriptorDescription
{
    uint32_t count;
    vk::DescriptorType type;
    vk::ShaderStageFlags stageFlags;
    vk::DescriptorBindingFlags bindingFlags;
};

using DescriptorSetDescription = std::vector<DescriptorDescription>;

using ImageInfo = std::vector<vk::DescriptorImageInfo>;
using BufferInfo = std::vector<vk::DescriptorBufferInfo>;
using BufferViews = std::vector<vk::BufferView>;
using AccelerationStructureInfo = vk::WriteDescriptorSetAccelerationStructureKHR;

using DescriptorInfo = std::variant<ImageInfo, BufferInfo, BufferViews, AccelerationStructureInfo>;

struct DescriptorData
{
    vk::DescriptorType type;
    DescriptorInfo descriptorInfo;
};

using DescriptorSetData = std::vector<DescriptorData>;

struct DescriptorSet
{
    vk::DescriptorSetLayout layout;
    vk::DescriptorSet value;
};

struct MultiDescriptorSet
{
    vk::DescriptorSetLayout layout;
    std::vector<vk::DescriptorSet> values;
};

namespace DescriptorHelpers
{
    DescriptorData GetData(vk::Sampler sampler, vk::ImageView view);

    DescriptorData GetData(vk::Sampler sampler, const std::vector<vk::ImageView>& views);

    DescriptorData GetData(const std::vector<SampledTexture>& textures);

    DescriptorData GetData(vk::Buffer buffer);

    DescriptorData GetStorageData(vk::ImageView view);

    DescriptorData GetStorageData(const std::vector<vk::ImageView>& views);

    DescriptorData GetStorageData(vk::Buffer buffer);

    DescriptorData GetStorageData(const std::vector<vk::Buffer>& buffers);

    DescriptorData GetData(const vk::AccelerationStructureKHR& accelerationStructure);

    DescriptorSet CreateDescriptorSet(const DescriptorSetDescription& description,
            const DescriptorSetData& descriptorSetData);

    MultiDescriptorSet CreateMultiDescriptorSet(const DescriptorSetDescription& description,
            const std::vector<DescriptorSetData>& multiDescriptorSetData);

    void DestroyDescriptorSet(const DescriptorSet& descriptorSet);

    void DestroyMultiDescriptorSet(const MultiDescriptorSet& multiDescriptorSet);
}
