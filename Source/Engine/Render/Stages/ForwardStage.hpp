#pragma once

#include "Engine/Render/RenderHelpers.hpp"
#include "Engine/Render/Vulkan/DescriptorHelpers.hpp"

class Scene;
class Scene;
class RenderPass;
class GraphicsPipeline;
struct KeyInput;

class ForwardStage
{
public:
    ForwardStage(vk::ImageView depthImageView);

    ~ForwardStage();

    void RegisterScene(const Scene* scene_);

    void RemoveScene();

    void Execute(vk::CommandBuffer commandBuffer, uint32_t imageIndex) const;

    void Resize(vk::ImageView depthImageView);

    void ReloadShaders();

private:
    struct EnvironmentData
    {
        vk::Buffer indexBuffer;
        DescriptorSet descriptorSet;
    };

    struct LightVolumeData
    {
        uint32_t positionsIndexCount = 0;
        uint32_t positionsInstanceCount = 0;
        uint32_t edgesIndexCount = 0;

        vk::Buffer positionsIndexBuffer;
        vk::Buffer positionsVertexBuffer;
        vk::Buffer positionsInstanceBuffer;
        vk::Buffer edgesIndexBuffer;

        DescriptorSet positionsDescriptorSet;
    };

    static EnvironmentData CreateEnvironmentData(const Scene& scene);
    static LightVolumeData CreateLightVolumeData(const Scene& scene);

    const Scene* scene = nullptr;

    std::unique_ptr<RenderPass> renderPass;
    std::vector<vk::Framebuffer> framebuffers;

    CameraData defaultCameraData;
    CameraData environmentCameraData;

    EnvironmentData environmentData;
    LightVolumeData lightVolumeData;

    std::unique_ptr<GraphicsPipeline> environmentPipeline;
    std::unique_ptr<GraphicsPipeline> lightVolumePositionsPipeline;
    std::unique_ptr<GraphicsPipeline> lightVolumeEdgesPipeline;

    bool drawLightVolume = false;

    std::vector<vk::DescriptorSetLayout> GetEnvironmentDescriptorSetLayout() const;
    std::vector<vk::DescriptorSetLayout> GetLightVolumeDescriptorSetLayout() const;

    void DrawEnvironment(vk::CommandBuffer commandBuffer, uint32_t imageIndex) const;
    void DrawLightVolume(vk::CommandBuffer commandBuffer, uint32_t imageIndex) const;

    void HandleKeyInputEvent(const KeyInput& keyInput);
};
