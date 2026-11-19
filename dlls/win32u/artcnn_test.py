#!/usr/bin/env python3
"""Optional offscreen Vulkan regression for the embedded ArtCNN C4F16 passes.

Developer-only utility; it is not a Wine build or test-suite dependency. Needs
Python 3.9+, NumPy 1.20+, ONNX, Python vulkan bindings, and a Vulkan 1.0
compute device. Downloads are intentionally not performed automatically.

Reference model: Artoriuz/ArtCNN at the commit and SHA-256 in this script.
Download ONNX/ArtCNN_C4F16.onnx and pass its path with --onnx. Examples:

  python3 dlls/win32u/artcnn_test.py --onnx /path/to/ArtCNN_C4F16.onnx
  python3 dlls/win32u/artcnn_test.py --onnx /path/to/ArtCNN_C4F16.onnx --input-size 8x9 --output-size 16x18
"""

import argparse
import hashlib
import pathlib
import re
import struct
import sys

HERE = pathlib.Path(__file__).resolve().parent
UPSTREAM_COMMIT = 'a1b6ace035df4a9e9a113713dd7ee4eb92dd67ea'
UPSTREAM_MODEL = (
    'https://github.com/Artoriuz/ArtCNN/blob/' + UPSTREAM_COMMIT +
    '/ONNX/ArtCNN_C4F16.onnx'
)
MODEL_SHA256 = 'ab5b82d6da9a5f44431b84f1065ff75f5a263ee608c997d82a412283ede63ee9'
UPSTREAM_WEIGHTS = (
    ('functional_1/conv2d_1/convolution/ReadVariableOp:0', 'functional_1/conv2d_1/Reshape:0'),
    ('functional_1/conv2d_1_2/convolution/ReadVariableOp:0', 'functional_1/conv2d_1_2/Reshape:0'),
    ('functional_1/conv2d_2_1/convolution/ReadVariableOp:0', 'functional_1/conv2d_2_1/Reshape:0'),
    ('functional_1/conv2d_3_1/convolution/ReadVariableOp:0', 'functional_1/conv2d_3_1/Reshape:0'),
    ('functional_1/conv2d_4_1/convolution/ReadVariableOp:0', 'functional_1/conv2d_4_1/Reshape:0'),
    ('functional_1/conv2d_5_1/convolution/ReadVariableOp:0', 'functional_1/conv2d_5_1/Reshape:0'),
    ('functional_1/conv2d_6_1/convolution/ReadVariableOp:0', 'functional_1/conv2d_6_1/Reshape:0'),
)
STAGES = ('conv0', 'conv1', 'conv2', 'conv3', 'conv4', 'conv5', 'conv6', 'reconstruct')


def dimensions(value):
    """Parse positive WIDTHxHEIGHT dimensions for argparse."""
    match = re.fullmatch(r'([1-9][0-9]*)[xX]([1-9][0-9]*)', value)
    if not match:
        raise argparse.ArgumentTypeError('expected positive WIDTHxHEIGHT (for example 9x11)')
    return tuple(map(int, match.groups()))


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--onnx', type=pathlib.Path,
                        default=HERE / 'ArtCNN_C4F16.onnx',
                        help='pinned upstream ONNX reference (default: %(default)s)')
    parser.add_argument('--spv-header', type=pathlib.Path,
                        default=HERE / 'artcnn_spv.h',
                        help='Wine SPIR-V header to execute (default: %(default)s)')
    parser.add_argument('--input-size', type=dimensions, default=(9, 11), metavar='WIDTHxHEIGHT',
                        help='BGRA8 input size; defaults to 9x11 (odd-size coverage)')
    parser.add_argument('--output-size', type=dimensions, default=(17, 19), metavar='WIDTHxHEIGHT',
                        help='BGRA8 output size; defaults to 17x19')
    return parser.parse_args(argv)


def load_modules(header):
    text = header.read_text(encoding='utf-8')
    modules = {}
    pattern = re.compile(
        r'static\s+const\s+uint32_t\s+artcnn_(\w+)_comp_spv\[\]\s*=\s*\{(.*?)\};',
        re.S,
    )
    for name, body in pattern.findall(text):
        if name in modules:
            raise ValueError(f'duplicate SPIR-V module {name!r} in {header}')
        words = [int(word, 16) for word in re.findall(r'0x[0-9a-fA-F]+', body)]
        if not words or words[0] != 0x07230203:
            raise ValueError(f'{name}: missing SPIR-V magic word')
        modules[name] = struct.pack('<' + 'I' * len(words), *words)
    if tuple(modules) != STAGES:
        raise ValueError(f'{header}: expected modules in order {STAGES}, found {tuple(modules)}')
    return [modules[name] for name in STAGES]


def load_weights(model_path):
    import onnx

    data = model_path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != MODEL_SHA256:
        raise ValueError(
            f'{model_path}: SHA-256 {digest} does not match pinned upstream model '
            f'{MODEL_SHA256} ({UPSTREAM_MODEL})'
        )
    model = onnx.load_model_from_string(data)
    source = {item.name: onnx.numpy_helper.to_array(item) for item in model.graph.initializer}
    expected = {f'kernel_oih_{i}' for i in range(7)} | {f'const_{i}' for i in range(1, 14, 2)}
    if expected.issubset(source):
        return source
    # Normalize the original upstream Keras-export initializer names to the
    # stable names used by the checked-in weight extraction script.
    weights = {}
    for index, (kernel_name, bias_name) in enumerate(UPSTREAM_WEIGHTS):
        if kernel_name not in source or bias_name not in source:
            raise ValueError(f'{model_path}: missing upstream convolution initializer {kernel_name}')
        weights[f'kernel_oih_{index}'] = source[kernel_name]
        weights[f'const_{1 + 2 * index}'] = source[bias_name]
    return weights


def memory_type(memtypes, requirements, flags):
    for index, item in enumerate(memtypes):
        if requirements.memoryTypeBits & (1 << index) and item.propertyFlags & flags == flags:
            return index
    available = ', '.join(f'{i}:0x{item.propertyFlags:x}' for i, item in enumerate(memtypes))
    raise RuntimeError(f'no Vulkan memory type satisfies 0x{flags:x} '
                       f'(memory type bits 0x{requirements.memoryTypeBits:x}; available {available})')


def bilinear(pixels, width, height):
    """Pixel-center bilinear sampling with clamped edge coordinates."""
    import numpy as np

    ph, pw = pixels.shape[:2]
    ry = (np.arange(height, dtype=np.float64) + 0.5) * ph / height - 0.5
    rx = (np.arange(width, dtype=np.float64) + 0.5) * pw / width - 0.5
    result = np.empty((height, width) + pixels.shape[2:], dtype=np.float64)
    for y, yy in enumerate(ry):
        iy = int(np.floor(yy))
        fy = yy - iy
        y0, y1 = np.clip((iy, iy + 1), 0, ph - 1)
        for x, xx in enumerate(rx):
            ix = int(np.floor(xx))
            fx = xx - ix
            x0, x1 = np.clip((ix, ix + 1), 0, pw - 1)
            result[y, x] = (
                pixels[y0, x0] * ((1 - fx) * (1 - fy)) +
                pixels[y0, x1] * (fx * (1 - fy)) +
                pixels[y1, x0] * ((1 - fx) * fy) +
                pixels[y1, x1] * (fx * fy)
            )
    return result


def cpu_reference(bgra, weights, output_width, output_height):
    import numpy as np

    height, width = bgra.shape[:2]
    coeff = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    rgb = bgra[:, :, [2, 1, 0, 3]].astype(np.float32) / 255
    cur = (rgb[:, :, :3] @ coeff)[:, :, None]
    actual = []
    for stage in range(7):
        if stage == 6:
            cur = cur + skip
        padded = np.pad(cur, ((1, 1), (1, 1), (0, 0)))
        view = np.lib.stride_tricks.sliding_window_view(padded, (3, 3), axis=(0, 1))
        cur = np.einsum('hwijl,oijl->hwo', view, weights[f'kernel_oih_{stage}'])
        cur += weights[f'const_{1 + 2 * stage}']
        if 1 <= stage <= 4:
            np.maximum(cur, 0, out=cur)
        cur = cur.astype(np.float16).astype(np.float32)
        if stage == 0:
            skip = cur.copy()
        actual.append(cur)
    # Final shader samples the four packed output channels as a 2x luma image.
    upscaled_luma = np.clip(
        cur.reshape(height, width, 2, 2).transpose(0, 2, 1, 3).reshape(height * 2, width * 2),
        0, 1,
    )
    luma = bilinear(upscaled_luma, output_width, output_height)
    base = bilinear(rgb, output_width, output_height)
    expected = np.round(255 * np.concatenate((
        np.clip(base[:, :, :3] + (luma - base[:, :, :3] @ coeff)[:, :, None], 0, 1),
        base[:, :, 3:],
    ), axis=2))[:, :, [2, 1, 0, 3]]
    return actual, expected


def run(args):
    import numpy as np
    import vulkan as vk

    width, height = args.input_size
    output_width, output_height = args.output_size
    modules = load_modules(args.spv_header)
    weights = load_weights(args.onnx)
    rng = np.random.default_rng(8)
    bgra = rng.integers(0, 256, size=(height, width, 4), dtype=np.uint8)

    instance = device = pool = cmd = None
    pipeline_layout = descriptor_layout = descriptor_pool = sampler = None
    buffers, images, shader_modules, pipelines = [], [], [], []
    try:
        app_info = vk.VkApplicationInfo(apiVersion=vk.VK_MAKE_VERSION(1, 0, 0))
        instance = vk.vkCreateInstance(vk.VkInstanceCreateInfo(pApplicationInfo=app_info), None)
        physical_devices = vk.vkEnumeratePhysicalDevices(instance)
        if not physical_devices:
            raise RuntimeError('Vulkan reported no physical devices')
        physical = physical_devices[0]
        queue_props = vk.vkGetPhysicalDeviceQueueFamilyProperties(physical)
        family = next((i for i, prop in enumerate(queue_props)
                       if prop.queueFlags & vk.VK_QUEUE_COMPUTE_BIT), None)
        if family is None:
            raise RuntimeError('the first Vulkan device has no compute queue family')
        # Keep the parent struct alive: CFFI's embedded fixed-size array may
        # otherwise outlive the temporary returned by this getter.
        memprops = vk.vkGetPhysicalDeviceMemoryProperties(physical)
        memtypes = memprops.memoryTypes
        features = vk.VkPhysicalDeviceFeatures(shaderStorageImageWriteWithoutFormat=True)
        queue_info = vk.VkDeviceQueueCreateInfo(queueFamilyIndex=family, pQueuePriorities=[1.0])
        device = vk.vkCreateDevice(physical, vk.VkDeviceCreateInfo(
            pQueueCreateInfos=[queue_info], pEnabledFeatures=features), None)
        queue = vk.vkGetDeviceQueue(device, family, 0)
        aspect = vk.VK_IMAGE_ASPECT_COLOR_BIT
        subresource = vk.VkImageSubresourceRange(aspectMask=aspect, levelCount=1, layerCount=1)
        layers = vk.VkImageSubresourceLayers(aspectMask=aspect, layerCount=1)
        general = vk.VK_IMAGE_LAYOUT_GENERAL
        pool = vk.vkCreateCommandPool(device, vk.VkCommandPoolCreateInfo(queueFamilyIndex=family), None)
        cmd = vk.vkAllocateCommandBuffers(device, vk.VkCommandBufferAllocateInfo(
            commandPool=pool, level=vk.VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            commandBufferCount=1))[0]
        vk.vkBeginCommandBuffer(cmd, vk.VkCommandBufferBeginInfo(
            flags=vk.VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT))

        def allocate(requirements, flags):
            index = memory_type(memtypes, requirements, flags)
            return vk.vkAllocateMemory(device, vk.VkMemoryAllocateInfo(
                allocationSize=requirements.size, memoryTypeIndex=index), None)

        def make_buffer(size):
            buffer = vk.vkCreateBuffer(device, vk.VkBufferCreateInfo(
                size=size, usage=vk.VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                vk.VK_BUFFER_USAGE_TRANSFER_DST_BIT), None)
            buffers.append([buffer, None])
            memory = allocate(vk.vkGetBufferMemoryRequirements(device, buffer),
                              vk.VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              vk.VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
            buffers[-1][1] = memory
            vk.vkBindBufferMemory(device, buffer, memory, 0)
            return buffer, memory

        def barrier(src, dst):
            stage_mask = vk.VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
            vk.vkCmdPipelineBarrier(cmd, stage_mask, stage_mask, 0, 1, [
                vk.VkMemoryBarrier(srcAccessMask=src, dstAccessMask=dst)],
                0, None, 0, None)

        def make_image(image_width, image_height, fmt):
            image = vk.vkCreateImage(device, vk.VkImageCreateInfo(
                imageType=vk.VK_IMAGE_TYPE_2D, format=fmt,
                extent=vk.VkExtent3D(width=image_width, height=image_height, depth=1),
                mipLevels=1, arrayLayers=1, samples=vk.VK_SAMPLE_COUNT_1_BIT,
                tiling=vk.VK_IMAGE_TILING_OPTIMAL,
                usage=vk.VK_IMAGE_USAGE_TRANSFER_SRC_BIT | vk.VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      vk.VK_IMAGE_USAGE_SAMPLED_BIT | vk.VK_IMAGE_USAGE_STORAGE_BIT), None)
            images.append([image, None, None])
            memory = allocate(vk.vkGetImageMemoryRequirements(device, image), 0)
            images[-1][2] = memory
            vk.vkBindImageMemory(device, image, memory, 0)
            view = vk.vkCreateImageView(device, vk.VkImageViewCreateInfo(
                image=image, viewType=vk.VK_IMAGE_VIEW_TYPE_2D, format=fmt,
                subresourceRange=subresource), None)
            images[-1][1] = view
            image_barrier = vk.VkImageMemoryBarrier(
                dstAccessMask=vk.VK_ACCESS_MEMORY_READ_BIT | vk.VK_ACCESS_MEMORY_WRITE_BIT,
                oldLayout=vk.VK_IMAGE_LAYOUT_UNDEFINED, newLayout=general,
                srcQueueFamilyIndex=vk.VK_QUEUE_FAMILY_IGNORED,
                dstQueueFamilyIndex=vk.VK_QUEUE_FAMILY_IGNORED,
                image=image, subresourceRange=subresource)
            stage_mask = vk.VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
            vk.vkCmdPipelineBarrier(cmd, stage_mask, stage_mask, 0, 0, None, 0, None,
                                   1, [image_barrier])
            return image, view

        def image_copy(image_width, image_height):
            return vk.VkBufferImageCopy(imageSubresource=layers,
                imageExtent=vk.VkExtent3D(width=image_width, height=image_height, depth=1))

        bgra8, rgba16f = vk.VK_FORMAT_B8G8R8A8_UNORM, vk.VK_FORMAT_R16G16B16A16_SFLOAT
        original = make_image(width, height, bgra8)
        skip, scratch_a, scratch_b = (make_image(2 * width, 2 * height, rgba16f)
                                      for _ in range(3))
        output = make_image(output_width, output_height, bgra8)
        upload, upload_mem = make_buffer(bgra.nbytes)
        vk.vkMapMemory(device, upload_mem, 0, bgra.nbytes, 0)[:] = bgra.tobytes()
        vk.vkUnmapMemory(device, upload_mem)
        vk.vkCmdCopyBufferToImage(cmd, upload, original[0], general, 1,
                                  [image_copy(width, height)])
        barrier(vk.VK_ACCESS_TRANSFER_WRITE_BIT, vk.VK_ACCESS_SHADER_READ_BIT)

        compute = vk.VK_SHADER_STAGE_COMPUTE_BIT
        combined, storage = vk.VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, vk.VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
        descriptor_layout = vk.vkCreateDescriptorSetLayout(device,
            vk.VkDescriptorSetLayoutCreateInfo(pBindings=[
                vk.VkDescriptorSetLayoutBinding(binding=i, descriptorType=kind,
                                                descriptorCount=1, stageFlags=compute)
                for i, kind in enumerate((combined, storage, combined))]), None)
        pipeline_layout = vk.vkCreatePipelineLayout(device, vk.VkPipelineLayoutCreateInfo(
            pSetLayouts=[descriptor_layout], pPushConstantRanges=[
                vk.VkPushConstantRange(stageFlags=compute, offset=0, size=16)]), None)
        descriptor_pool = vk.vkCreateDescriptorPool(device, vk.VkDescriptorPoolCreateInfo(
            maxSets=8, pPoolSizes=[vk.VkDescriptorPoolSize(type=combined, descriptorCount=16),
                                   vk.VkDescriptorPoolSize(type=storage, descriptorCount=8)]), None)
        sets = vk.vkAllocateDescriptorSets(device, vk.VkDescriptorSetAllocateInfo(
            descriptorPool=descriptor_pool, pSetLayouts=[descriptor_layout] * 8))
        sampler = vk.vkCreateSampler(device, vk.VkSamplerCreateInfo(
            magFilter=vk.VK_FILTER_NEAREST, minFilter=vk.VK_FILTER_NEAREST), None)
        push = vk.ffi.new('uint32_t[4]', [width, height, output_width, output_height])
        readbacks = []
        pass_images = ((original, skip, original), (skip, scratch_a, original),
                       (scratch_a, scratch_b, original), (scratch_b, scratch_a, original),
                       (scratch_a, scratch_b, original), (scratch_b, scratch_a, original),
                       (scratch_a, scratch_b, skip), (scratch_b, output, original))
        for stage_index, (src, dst, extra) in enumerate(pass_images):
            module = vk.vkCreateShaderModule(device, vk.VkShaderModuleCreateInfo(
                codeSize=len(modules[stage_index]), pCode=modules[stage_index]), None)
            shader_modules.append(module)
            shader_stage = vk.VkPipelineShaderStageCreateInfo(
                stage=compute, module=module, pName='main')
            pipeline = vk.vkCreateComputePipelines(device, vk.VK_NULL_HANDLE, 1, [
                vk.VkComputePipelineCreateInfo(stage=shader_stage, layout=pipeline_layout,
                                              basePipelineIndex=-1)], None)[0]
            pipelines.append(pipeline)
            writes = []
            for binding, (image_desc, kind) in enumerate(zip(
                    (src, dst, extra), (combined, storage, combined))):
                info = vk.VkDescriptorImageInfo(
                    sampler=sampler if kind == combined else vk.VK_NULL_HANDLE,
                    imageView=image_desc[1], imageLayout=general)
                writes.append(vk.VkWriteDescriptorSet(dstSet=sets[stage_index],
                    dstBinding=binding, descriptorCount=1, descriptorType=kind, pImageInfo=[info]))
            vk.vkUpdateDescriptorSets(device, len(writes), writes, 0, None)
            vk.vkCmdBindPipeline(cmd, vk.VK_PIPELINE_BIND_POINT_COMPUTE, pipeline)
            vk.vkCmdBindDescriptorSets(cmd, vk.VK_PIPELINE_BIND_POINT_COMPUTE,
                                       pipeline_layout, 0, 1, [sets[stage_index]], 0, None)
            vk.vkCmdPushConstants(cmd, pipeline_layout, compute, 0, 16, push)
            dispatch_width, dispatch_height = ((output_width, output_height) if stage_index == 7
                                               else (width, height))
            vk.vkCmdDispatch(cmd, (dispatch_width + 7) // 8, (dispatch_height + 7) // 8, 1)
            barrier(vk.VK_ACCESS_SHADER_WRITE_BIT,
                    vk.VK_ACCESS_SHADER_READ_BIT | vk.VK_ACCESS_TRANSFER_READ_BIT)
            read_width, read_height, bytes_per_pixel = (
                (output_width, output_height, 4) if stage_index == 7 else
                (width, height, 8) if stage_index == 6 else
                (2 * width, 2 * height, 8))
            size = read_width * read_height * bytes_per_pixel
            read_buffer, read_memory = make_buffer(size)
            vk.vkCmdCopyImageToBuffer(cmd, dst[0], general, read_buffer, 1,
                                      [image_copy(read_width, read_height)])
            readbacks.append((read_memory, read_width, read_height, bytes_per_pixel, size))
            barrier(vk.VK_ACCESS_TRANSFER_READ_BIT | vk.VK_ACCESS_SHADER_READ_BIT,
                    vk.VK_ACCESS_SHADER_WRITE_BIT)
        barrier(vk.VK_ACCESS_TRANSFER_WRITE_BIT, vk.VK_ACCESS_HOST_READ_BIT)
        vk.vkEndCommandBuffer(cmd)
        vk.vkQueueSubmit(queue, 1, [vk.VkSubmitInfo(pCommandBuffers=[cmd])], vk.VK_NULL_HANDLE)
        vk.vkQueueWaitIdle(queue)

        actual = []
        for memory, read_width, read_height, bytes_per_pixel, size in readbacks:
            mapped = vk.vkMapMemory(device, memory, 0, size, 0)
            dtype = np.float16 if bytes_per_pixel == 8 else np.uint8
            pixels = np.frombuffer(mapped, dtype=dtype).reshape(read_height, read_width, 4).copy()
            vk.vkUnmapMemory(device, memory)
            if bytes_per_pixel == 8 and len(actual) < 6:
                pixels = pixels.reshape(height, 2, width, 2, 4).transpose(0, 2, 1, 3, 4).reshape(height, width, 16)
            actual.append(pixels)

        expected_stages, expected_final = cpu_reference(bgra, weights, output_width, output_height)
        for index, (observed, expected) in enumerate(zip(actual[:7], expected_stages)):
            error = float(np.max(np.abs(observed.astype(np.float32) - expected)))
            print(f'stage {index} max absolute error {error:g}')
            if error >= 0.007:
                raise AssertionError(f'stage {index} error {error:g} exceeds 0.007')
        final_error = int(np.max(np.abs(actual[7].astype(np.int16) - expected_final.astype(np.int16))))
        print(f'final {width}x{height} -> {output_width}x{output_height}, '
              f'max absolute BGRA8 error {final_error}')
        if final_error > 2:
            raise AssertionError(f'final error {final_error} exceeds 2 BGRA8 code values')
    finally:
        if device is not None:
            try:
                vk.vkDeviceWaitIdle(device)
            except Exception:
                pass
            if pool is not None:
                vk.vkDestroyCommandPool(device, pool, None)
            for pipeline in pipelines:
                vk.vkDestroyPipeline(device, pipeline, None)
            for module in shader_modules:
                vk.vkDestroyShaderModule(device, module, None)
            if pipeline_layout is not None:
                vk.vkDestroyPipelineLayout(device, pipeline_layout, None)
            if descriptor_pool is not None:
                vk.vkDestroyDescriptorPool(device, descriptor_pool, None)
            if descriptor_layout is not None:
                vk.vkDestroyDescriptorSetLayout(device, descriptor_layout, None)
            if sampler is not None:
                vk.vkDestroySampler(device, sampler, None)
            for image, view, memory in images:
                if view is not None:
                    vk.vkDestroyImageView(device, view, None)
                vk.vkDestroyImage(device, image, None)
                if memory is not None:
                    vk.vkFreeMemory(device, memory, None)
            for buffer, memory in buffers:
                vk.vkDestroyBuffer(device, buffer, None)
                if memory is not None:
                    vk.vkFreeMemory(device, memory, None)
            vk.vkDestroyDevice(device, None)
        if instance is not None:
            vk.vkDestroyInstance(instance, None)


def main(argv=None):
    args = arguments(argv)
    try:
        run(args)
    except Exception as error:
        print(f'artcnn_test.py: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
