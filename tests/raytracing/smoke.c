/* Offscreen RT test: build BLAS/TLAS, trace hit and miss rays, verify GPU output.
 * Each invocation loads exactly one ICD. Run under a process timeout. */
#define VK_NO_PROTOTYPES
#include <hardware/hwvulkan.h>
#include <dlfcn.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    fprintf(stderr,"FAIL %s: %d\n", #call, r_); exit(2); } } while(0)
#define FUNCTIONS(X) \
 X(CreateBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(BindBufferMemory) \
 X(MapMemory) X(GetBufferDeviceAddress) X(CreateAccelerationStructureKHR) \
 X(GetAccelerationStructureBuildSizesKHR) X(GetAccelerationStructureDeviceAddressKHR) \
 X(CmdBuildAccelerationStructuresKHR) X(CreateCommandPool) X(AllocateCommandBuffers) \
 X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(GetDeviceQueue) \
 X(QueueSubmit) X(CreateFence) X(WaitForFences) X(DestroyFence) X(CreateDescriptorSetLayout) \
 X(CreateDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreatePipelineLayout) \
 X(CreateShaderModule) X(CreateComputePipelines) X(CmdBindPipeline) X(CmdBindDescriptorSets) \
 X(CmdDispatch) X(DestroyDevice) X(DeviceWaitIdle) X(DestroyBuffer) X(FreeMemory) \
 X(UnmapMemory) X(DestroyAccelerationStructureKHR) X(DestroyCommandPool) \
 X(DestroyDescriptorPool) X(DestroyDescriptorSetLayout) X(DestroyPipelineLayout) \
 X(DestroyPipeline) X(DestroyShaderModule)
#define DECL(n) static PFN_vk##n vk##n;
FUNCTIONS(DECL)
static PFN_vkCreateRayTracingPipelinesKHR vkCreateRayTracingPipelinesKHR;
static PFN_vkGetRayTracingShaderGroupHandlesKHR vkGetRayTracingShaderGroupHandlesKHR;
static PFN_vkCmdTraceRaysKHR vkCmdTraceRaysKHR;
static VkDevice device;
static VkPhysicalDeviceMemoryProperties memory_props;
static VkQueue queue;
static VkCommandPool pool;
static uint32_t scratch_alignment;
typedef struct { VkBuffer buffer; VkDeviceMemory memory; VkDeviceAddress address; void *map; } Buffer;
static Buffer buffers[16];
static unsigned buffer_count;
static VkShaderModule modules[3];
static unsigned module_count;
static VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}
static Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, int mapped) {
    Buffer b = {0};
    VkBufferCreateInfo ci = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size=size,
        .usage=usage|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(device,&ci,NULL,&b.buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(device,b.buffer,&req);
    VkMemoryPropertyFlags flags = mapped ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type;
    for(type=0;type<memory_props.memoryTypeCount;type++)
        if ((req.memoryTypeBits & (1u<<type)) && (memory_props.memoryTypes[type].propertyFlags & flags)==flags) break;
    if(type==memory_props.memoryTypeCount) { fprintf(stderr,"No suitable memory type\n"); exit(2); }
    VkMemoryAllocateFlagsInfo af={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&af,.allocationSize=req.size,.memoryTypeIndex=type};
    CHECK(vkAllocateMemory(device,&ai,NULL,&b.memory));
    CHECK(vkBindBufferMemory(device,b.buffer,b.memory,0));
    VkBufferDeviceAddressInfo address={.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,.buffer=b.buffer};
    b.address=vkGetBufferDeviceAddress(device,&address);
    if(mapped) CHECK(vkMapMemory(device,b.memory,0,VK_WHOLE_SIZE,0,&b.map));
    if(buffer_count>=16) exit(2);
    buffers[buffer_count++]=b;
    return b;
}
static VkCommandBuffer begin(void) {
    VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(device,&ai,&cmd));
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(cmd,&bi)); return cmd;
}
static void finish(VkCommandBuffer cmd) {
    CHECK(vkEndCommandBuffer(cmd));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; CHECK(vkCreateFence(device,&fi,NULL,&fence));
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    CHECK(vkQueueSubmit(queue,1,&si,fence));
    CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull));
    vkDestroyFence(device,fence,NULL);
}
static VkAccelerationStructureKHR build(VkAccelerationStructureTypeKHR type, VkAccelerationStructureGeometryKHR *geo) {
    VkAccelerationStructureBuildGeometryInfoKHR bi={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type=type,.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
        .mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,.geometryCount=1,.pGeometries=geo};
    uint32_t count=1;
    VkAccelerationStructureBuildSizesInfoKHR size={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&bi,&count,&size);
    Buffer storage=buffer(size.accelerationStructureSize,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,0);
    Buffer scratch=buffer(size.buildScratchSize+scratch_alignment,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,0);
    VkAccelerationStructureCreateInfoKHR ci={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer=storage.buffer,.size=size.accelerationStructureSize,.type=type};
    VkAccelerationStructureKHR as; CHECK(vkCreateAccelerationStructureKHR(device,&ci,NULL,&as));
    bi.dstAccelerationStructure=as; bi.scratchData.deviceAddress=align_up(scratch.address,scratch_alignment);
    VkAccelerationStructureBuildRangeInfoKHR range={.primitiveCount=1};
    const VkAccelerationStructureBuildRangeInfoKHR *ranges=&range;
    VkCommandBuffer cmd=begin();
    vkCmdBuildAccelerationStructuresKHR(cmd,1,&bi,&ranges);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR|VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&barrier,0,NULL,0,NULL);
    finish(cmd); printf("built %s size=%llu\n",type==VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR?"BLAS":"TLAS",(unsigned long long)size.accelerationStructureSize);
    return as;
}
static VkShaderModule shader(const char *dir, const char *name) {
    char path[1024]; if(snprintf(path,sizeof(path),"%s/%s.spv",dir,name)>=(int)sizeof(path)) exit(2);
    FILE *f=fopen(path,"rb"); if(!f) { perror(path); exit(2); }
    if(fseek(f,0,SEEK_END)) exit(2);
    long size=ftell(f); if(size<=0 || size%4 || fseek(f,0,SEEK_SET)) exit(2);
    uint32_t *code=malloc(size); if(!code || fread(code,1,size,f)!=(size_t)size) exit(2); fclose(f);
    VkShaderModuleCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=size,.pCode=code};
    VkShaderModule module; CHECK(vkCreateShaderModule(device,&ci,NULL,&module)); free(code);
    if(module_count>=3) exit(2);
    modules[module_count++]=module;
    return module;
}
int main(int argc, char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    if(argc<4 || argc>6 || (strcmp(argv[3],"query") && strcmp(argv[3],"pipeline") && strcmp(argv[3],"recursive") && strcmp(argv[3],"render"))) {
        fprintf(stderr,"Usage: %s HAL.so shader-dir query|pipeline|recursive|render [rays rounds]\n",argv[0]); return 2;
    }
    int rt=strcmp(argv[3],"query")!=0;
    int recursive=!strcmp(argv[3],"recursive");
    int render=!strcmp(argv[3],"render");
    uint32_t rays=argc>=5?(uint32_t)atoi(argv[4]):2;
    unsigned rounds=argc>=6?(unsigned)atoi(argv[5]):1;
    if(rays<2 || rays>65535 || rounds<1 || rounds>100) return 2;
    if(render) rays=128*96;
    void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); if(!lib) { fprintf(stderr,"%s\n",dlerror()); return 2; }
    hwvulkan_module_t *module=dlsym(lib,"HMI"); hw_device_t *hal=NULL;
    if(!module || module->common.methods->open(&module->common,"vk0",&hal)) return 2;
    PFN_vkGetInstanceProcAddr gipa=((hwvulkan_device_t*)hal)->GetInstanceProcAddr;
    PFN_vkCreateInstance create=(void*)gipa(NULL,"vkCreateInstance");
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Banners RT smoke",.apiVersion=VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance; CHECK(create(&ici,NULL,&instance));
#define IP(n) PFN_vk##n vk##n=(void*)gipa(instance,"vk"#n)
    IP(EnumeratePhysicalDevices); IP(GetPhysicalDeviceMemoryProperties); IP(GetPhysicalDeviceProperties2);
    IP(GetPhysicalDeviceQueueFamilyProperties); IP(EnumerateDeviceExtensionProperties);
    IP(GetPhysicalDeviceFeatures2); IP(CreateDevice); IP(GetDeviceProcAddr); IP(DestroyInstance);
    uint32_t count=1; VkPhysicalDevice physical; CHECK(vkEnumeratePhysicalDevices(instance,&count,&physical));
    uint32_t ne=0; CHECK(vkEnumerateDeviceExtensionProperties(physical,NULL,&ne,NULL));
    VkExtensionProperties *ext=calloc(ne,sizeof(*ext)); if(!ext) return 2;
    CHECK(vkEnumerateDeviceExtensionProperties(physical,NULL,&ne,ext));
    const char *extensions[]={VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        rt?VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME:VK_KHR_RAY_QUERY_EXTENSION_NAME};
    for(unsigned i=0;i<3;i++) {
        int found=0; for(uint32_t j=0;j<ne;j++) found |= !strcmp(extensions[i],ext[j].extensionName);
        if(!found) { printf("UNSUPPORTED %s\n",extensions[i]); return 3; }
    }
    free(ext);
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asp={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtp={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    if(rt) asp.pNext=&rtp;
    VkPhysicalDeviceProperties2 props={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&asp};
    vkGetPhysicalDeviceProperties2(physical,&props);
    printf("device=%s mode=%s\n",props.properties.deviceName,argv[3]);
    scratch_alignment=asp.minAccelerationStructureScratchOffsetAlignment;
    if(!scratch_alignment) return 2;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtf={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rqf={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .pNext=rt?(void*)&rtf:(void*)&rqf};
    VkPhysicalDeviceBufferDeviceAddressFeatures bda={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,.pNext=&asf};
    VkPhysicalDeviceFeatures2 features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&bda};
    vkGetPhysicalDeviceFeatures2(physical,&features);
    if(!bda.bufferDeviceAddress || !asf.accelerationStructure || (rt?!rtf.rayTracingPipeline:!rqf.rayQuery)) return 3;
    /* Enable only the features this test uses. */
    bda.bufferDeviceAddressCaptureReplay=VK_FALSE; bda.bufferDeviceAddressMultiDevice=VK_FALSE;
    memset(&asf,0,sizeof(asf)); asf.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    asf.pNext=rt?(void*)&rtf:(void*)&rqf; asf.accelerationStructure=VK_TRUE;
    memset(&rtf,0,sizeof(rtf)); rtf.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR; rtf.rayTracingPipeline=VK_TRUE;
    vkGetPhysicalDeviceMemoryProperties(physical,&memory_props);
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,NULL);
    VkQueueFamilyProperties *families=calloc(count,sizeof(*families)); if(!families) return 2;
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);
    uint32_t family; for(family=0;family<count;family++) if(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT) break;
    free(families); if(family==count) return 2;
    float priority=1; VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&bda,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,
        .enabledExtensionCount=3,.ppEnabledExtensionNames=extensions};
    CHECK(vkCreateDevice(physical,&dci,NULL,&device));
#define LOAD(n) vk##n=(void*)vkGetDeviceProcAddr(device,"vk"#n); if(!vk##n) { fprintf(stderr,"Missing vk%s\n",#n); return 2; }
    FUNCTIONS(LOAD)
    if(rt) { LOAD(CreateRayTracingPipelinesKHR) LOAD(GetRayTracingShaderGroupHandlesKHR) LOAD(CmdTraceRaysKHR) }
    vkGetDeviceQueue(device,family,0,&queue);
    VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    CHECK(vkCreateCommandPool(device,&pci,NULL,&pool));
    float vertices[]={-1,-1,0, 1,-1,0, 0,1,0};
    Buffer vb=buffer(sizeof(vertices),VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,1); memcpy(vb.map,vertices,sizeof(vertices));
    VkAccelerationStructureGeometryKHR geo={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR,.flags=VK_GEOMETRY_OPAQUE_BIT_KHR,
        .geometry.triangles={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
            .vertexFormat=VK_FORMAT_R32G32B32_SFLOAT,.vertexData.deviceAddress=vb.address,.vertexStride=12,.maxVertex=2,.indexType=VK_INDEX_TYPE_NONE_KHR}};
    VkAccelerationStructureKHR blas=build(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,&geo);
    VkAccelerationStructureDeviceAddressInfoKHR adi={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,.accelerationStructure=blas};
    VkAccelerationStructureInstanceKHR inst={.transform.matrix={{1,0,0,0},{0,1,0,0},{0,0,1,0}},.mask=255,
        .flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
        .accelerationStructureReference=vkGetAccelerationStructureDeviceAddressKHR(device,&adi)};
    Buffer ib=buffer(sizeof(inst),VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,1); memcpy(ib.map,&inst,sizeof(inst));
    VkAccelerationStructureGeometryKHR ig={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry.instances={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,.data.deviceAddress=ib.address}};
    VkAccelerationStructureKHR tlas=build(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,&ig);
    Buffer output=buffer(rays*sizeof(uint32_t),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,1);
    VkShaderStageFlags stages=rt?VK_SHADER_STAGE_RAYGEN_BIT_KHR|VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutBinding bindings[]={{.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,.descriptorCount=1,.stageFlags=stages},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=stages}};
    VkDescriptorSetLayoutCreateInfo dlci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
    VkDescriptorSetLayout set_layout; CHECK(vkCreateDescriptorSetLayout(device,&dlci,NULL,&set_layout));
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPoolCreateInfo dpci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes};
    VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(device,&dpci,NULL,&dp));
    VkDescriptorSetAllocateInfo dsai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=dp,.descriptorSetCount=1,.pSetLayouts=&set_layout};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device,&dsai,&set));
    VkWriteDescriptorSetAccelerationStructureKHR wa={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,.accelerationStructureCount=1,.pAccelerationStructures=&tlas};
    VkDescriptorBufferInfo obi={.buffer=output.buffer,.range=rays*sizeof(uint32_t)};
    VkWriteDescriptorSet writes[]={{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.pNext=&wa,.dstSet=set,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR},
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&obi}};
    vkUpdateDescriptorSets(device,2,writes,0,NULL);
    VkPipelineLayoutCreateInfo plci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(device,&plci,NULL,&layout));
    VkPipeline pipeline;
    VkStridedDeviceAddressRegionKHR regions[4]={{0}};
    if(rt) {
        const char *names[]={render?"render.rgen":"trace.rgen",render?"render.rmiss":"trace.rmiss",
            render?"render.rchit":recursive?"recursive.rchit":"trace.rchit"};
        VkShaderStageFlagBits bits[]={VK_SHADER_STAGE_RAYGEN_BIT_KHR,VK_SHADER_STAGE_MISS_BIT_KHR,VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR};
        VkPipelineShaderStageCreateInfo ss[3]; VkRayTracingShaderGroupCreateInfoKHR groups[3];
        for(unsigned i=0;i<3;i++) {
            ss[i]=(VkPipelineShaderStageCreateInfo){.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=bits[i],.module=shader(argv[2],names[i]),.pName="main"};
            groups[i]=(VkRayTracingShaderGroupCreateInfoKHR){.sType=VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                .type=i==2?VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR:VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
                .generalShader=i==2?VK_SHADER_UNUSED_KHR:i,.closestHitShader=i==2?i:VK_SHADER_UNUSED_KHR,
                .anyHitShader=VK_SHADER_UNUSED_KHR,.intersectionShader=VK_SHADER_UNUSED_KHR};
        }
        VkRayTracingPipelineCreateInfoKHR rpci={.sType=VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,.stageCount=3,.pStages=ss,
            .groupCount=3,.pGroups=groups,.maxPipelineRayRecursionDepth=recursive?2:1,.layout=layout};
        CHECK(vkCreateRayTracingPipelinesKHR(device,VK_NULL_HANDLE,VK_NULL_HANDLE,1,&rpci,NULL,&pipeline));
        uint32_t handle=rtp.shaderGroupHandleSize;
        VkDeviceSize stride=align_up(handle,rtp.shaderGroupHandleAlignment);
        VkDeviceSize spacing=align_up(stride,rtp.shaderGroupBaseAlignment);
        Buffer sbt=buffer(spacing*3+rtp.shaderGroupBaseAlignment,VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR,1);
        VkDeviceSize offset=align_up(sbt.address,rtp.shaderGroupBaseAlignment)-sbt.address;
        unsigned char *handles=malloc(handle*3); if(!handles) return 2;
        CHECK(vkGetRayTracingShaderGroupHandlesKHR(device,pipeline,0,3,handle*3,handles));
        for(unsigned i=0;i<3;i++) { memcpy((char*)sbt.map+offset+spacing*i,handles+handle*i,handle);
            regions[i]=(VkStridedDeviceAddressRegionKHR){sbt.address+offset+spacing*i,stride,stride}; }
        free(handles);
    } else {
        VkComputePipelineCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader(argv[2],"query.comp"),.pName="main"},.layout=layout};
        CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cpci,NULL,&pipeline));
    }
    uint32_t *result=output.map;
    int passed=1;
    uint32_t expected_hit=recursive?33:11;
    for(unsigned round=0;round<rounds;round++) {
        memset(output.map,0,rays*sizeof(uint32_t));
        VkCommandBuffer cmd=begin();
        VkPipelineBindPoint bind=rt?VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR:VK_PIPELINE_BIND_POINT_COMPUTE;
        vkCmdBindPipeline(cmd,bind,pipeline); vkCmdBindDescriptorSets(cmd,bind,layout,0,1,&set,0,NULL);
        if(rt) vkCmdTraceRaysKHR(cmd,&regions[0],&regions[1],&regions[2],&regions[3],rays,1,1);
        else vkCmdDispatch(cmd,rays,1,1);
        VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cmd,rt?VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR:VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
        finish(cmd);
        for(uint32_t i=0;i<rays;i++) {
            if(render) {
                float x=((i%128)+0.5f)/128*2-1, y=((i/128)+0.5f)/96*2-1;
                float v=(y+1)*0.5f, u=(x+1-v)*0.5f, w=1-u-v;
                uint32_t expected=0xff201810;
                if(u>=0 && v>=0 && w>=0)
                    expected=0xff000000 | (uint32_t)lroundf(w*255) | ((uint32_t)lroundf(u*255)<<8) | ((uint32_t)lroundf(v*255)<<16);
                int pixel_ok=1;
                for(unsigned c=0;c<4;c++) {
                    int a=(result[i]>>(c*8))&255, e=(expected>>(c*8))&255;
                    if(abs(a-e)>1) pixel_ok=0;
                }
                if(!pixel_ok) { fprintf(stderr,"FAIL round=%u pixel=%u value=0x%x expected=0x%x\n",round,i,result[i],expected); passed=0; break; }
                continue;
            }
            uint32_t expected=(i&1)?22:expected_hit;
            if(result[i]!=expected) {
                fprintf(stderr,"FAIL round=%u ray=%u value=%u expected=%u\n",round,i,result[i],expected);
                passed=0; break;
            }
        }
        if(!passed) break;
    }
    if(render) {
        char path[1024]; if(snprintf(path,sizeof(path),"%s/native-rt-render.ppm",argv[2])>=(int)sizeof(path)) return 2;
        FILE *f=fopen(path,"wb"); if(!f) { perror(path); return 2; }
        fprintf(f,"P6\n128 96\n255\n");
        for(int y=95;y>=0;y--) for(unsigned x=0;x<128;x++) {
            uint32_t pixel=result[y*128+x];
            unsigned char rgb[]={pixel&255,(pixel>>8)&255,(pixel>>16)&255};
            if(fwrite(rgb,1,3,f)!=3) return 2;
        }
        if(fclose(f)) return 2;
        printf("%s render pixels=%u rounds=%u image=%s\n",passed?"PASS":"FAIL",rays,rounds,path);
    } else printf("%s %s rays=%u rounds=%u hit=%u miss=%u expected=%u,22\n",passed?"PASS":"FAIL",argv[3],rays,rounds,result[0],result[1],expected_hit);
    CHECK(vkDeviceWaitIdle(device));
    vkDestroyCommandPool(device,pool,NULL);
    vkDestroyPipeline(device,pipeline,NULL);
    for(unsigned i=0;i<module_count;i++) vkDestroyShaderModule(device,modules[i],NULL);
    vkDestroyDescriptorPool(device,dp,NULL);
    vkDestroyPipelineLayout(device,layout,NULL);
    vkDestroyDescriptorSetLayout(device,set_layout,NULL);
    vkDestroyAccelerationStructureKHR(device,tlas,NULL);
    vkDestroyAccelerationStructureKHR(device,blas,NULL);
    for(unsigned i=0;i<buffer_count;i++) {
        if(buffers[i].map) vkUnmapMemory(device,buffers[i].memory);
        vkDestroyBuffer(device,buffers[i].buffer,NULL);
        vkFreeMemory(device,buffers[i].memory,NULL);
    }
    vkDestroyDevice(device,NULL); vkDestroyInstance(instance,NULL);
    if(hal->close) hal->close(hal);
    /* Direct HAL diagnostics bypass Android's loader lifetime management.
     * Reap vendor profiling threads with process exit rather than invoking
     * static DSO destructors while they are still using their mutexes. */
    fflush(NULL);
    _Exit(passed?0:1);
}
