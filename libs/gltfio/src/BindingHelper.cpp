/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gltfio/BindingHelper.h>

#include "FFilamentAsset.h"
#include "upcast.h"

#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/VertexBuffer.h>

#include <math/quat.h>
#include <math/vec3.h>
#include <math/vec4.h>

#include <utils/Log.h>

#include <cgltf.h>

#include <tsl/robin_map.h>

#include <string>

// TODO: to simplify the implementation of BindingHelper, we are using cgltf_load_buffer_base64 and
// cgltf_load_buffer_file, which are normally private to the library. We should consider
// substituting these functions with our own implementation since they are fairly simple.

using namespace filament;
using namespace filament::math;
using namespace utils;

namespace {
    using UrlMap = tsl::robin_map<std::string, const uint8_t*>;
}

namespace gltfio {

using namespace details;

class UrlCache {
public:
    void* getResource(const char* uri) {
        auto iter = mBlobs.find(uri);
        return (iter == mBlobs.end()) ? nullptr : iter->second;
    }

    void addResource(const char* uri, void* blob) {
        mBlobs[uri] = blob;
    }

    void addPendingUpload() {
        ++mPendingUploads;
    }

    UrlCache() {}

    ~UrlCache() {
        // TODO: free all mBlobs
    }

    // Destroy the URL cache only after the pending upload count is zero and the client has
    // destroyed the BindingHelper object.
    static void onLoadedResource(void* buffer, size_t size, void* user) {
        auto cache = (UrlCache*) user;
        if (--cache->mPendingUploads == 0 && cache->mOwnerDestroyed) {
            delete cache;
        }
    }

    void onOwnerDestroyed() {
        if (mPendingUploads == 0) {
            delete this;
        } else {
            mOwnerDestroyed = true;
        }
    }

private:
    bool mOwnerDestroyed = false;
    int mPendingUploads = 0;
    tsl::robin_map<std::string, void*> mBlobs; // TODO: can we simply use const char* for the key?
};

BindingHelper::BindingHelper(Engine* engine, const char* basePath) : mEngine(engine),
        mBasePath(basePath), mCache(new UrlCache) {}

BindingHelper::~BindingHelper() {
    mCache->onOwnerDestroyed();
}

bool BindingHelper::loadResources(FilamentAsset* asset) {
    const BufferBinding* bindings = asset->getBufferBindings();
    for (size_t i = 0, n = asset->getBufferBindingCount(); i < n; ++i) {
        auto bb = bindings[i];
        void* data = mCache->getResource(bb.uri);
        if (data) {
            // Do nothing.
        } else if (isBase64(bb)) {
            data = loadBase64(bb);
            mCache->addResource(bb.uri, data);
        } else if (isFile(bb)) {
            data = loadFile(bb);
            mCache->addResource(bb.uri, data);
        } else {
            slog.e << "Unable to obtain resource: " << bb.uri << io::endl;
            return false;
        }
        uint8_t* ucdata = bb.offset + (uint8_t*) data;
        if (bb.vertexBuffer) {
            mCache->addPendingUpload();
            VertexBuffer::BufferDescriptor bd(ucdata, bb.size, UrlCache::onLoadedResource, mCache);
            bb.vertexBuffer->setBufferAt(*mEngine, bb.bufferIndex, std::move(bd));
        } else if (bb.indexBuffer) {
            mCache->addPendingUpload();
            VertexBuffer::BufferDescriptor bd(ucdata, bb.size, UrlCache::onLoadedResource, mCache);
            bb.indexBuffer->setBuffer(*mEngine, std::move(bd));
        } else if (bb.animationBuffer) {
            memcpy(bb.animationBuffer, ucdata, bb.size);
        } else if (bb.orientationBuffer) {
            memcpy(bb.orientationBuffer, ucdata, bb.size);
        } else {
            slog.e << "Malformed binding: " << bb.uri << io::endl;
            return false;
        }
    }

    FFilamentAsset* fasset = upcast(asset);
    if (fasset->mOrientationBuffer.size() > 0) {
        computeTangents(fasset);
    }
    return true;
}

bool BindingHelper::isBase64(const BufferBinding& bb) {
   if (bb.uri && strncmp(bb.uri, "data:", 5) == 0) {
        const char* comma = strchr(bb.uri, ',');
        if (comma && comma - bb.uri >= 7 && strncmp(comma - 7, ";base64", 7) == 0) {
            return true;
        }
    }
    return false;
}

void* BindingHelper::loadBase64(const BufferBinding& bb) {
    if (!bb.uri || strncmp(bb.uri, "data:", 5)) {
        return nullptr;
    }
    const char* comma = strchr(bb.uri, ',');
    if (comma && comma - bb.uri >= 7 && strncmp(comma - 7, ";base64", 7) == 0) {
        cgltf_options options {};
        void* data = nullptr;
        cgltf_result result = cgltf_load_buffer_base64(
                &options, bb.totalSize, comma + 1, &data);
        if (result != cgltf_result_success) {
            slog.e << "Unable to parse base64 URL." << io::endl;
            return nullptr;
        }
        return data;
    }
    return nullptr;
}

bool BindingHelper::isFile(const BufferBinding& bb) {
    return strstr(bb.uri, "://") == nullptr;
}

void* BindingHelper::loadFile(const BufferBinding& bb) {
    cgltf_options options {};
    void* data = nullptr;
    cgltf_result result = cgltf_load_buffer_file(
            &options, bb.totalSize, bb.uri, mBasePath.c_str(), &data);
    if (result != cgltf_result_success) {
        slog.e << "Unable to consume " << bb.uri << io::endl;
        return nullptr;
    }
    return data;
}

void BindingHelper::computeTangents(const FFilamentAsset* asset) {
    const auto& nodeMap = asset->mNodeMap;

    UrlMap blobs; // TODO: can the key be const char* ?
    const BufferBinding* bindings = asset->getBufferBindings();
    for (size_t i = 0, n = asset->getBufferBindingCount(); i < n; ++i) {
        auto bb = bindings[i];
        if (bb.orientationBuffer) {
            blobs[bb.uri] = bb.orientationBuffer;
        }
    }

    // Declare a vector of quats (which we will populate via populateTangentQuaternions)
    // as well as vectors of normals and tangents (which we'll extract & convert from the source)
    std::vector<quath> fp16Quats;
    std::vector<float3> fp32Normals;
    std::vector<float4> fp32Tangents;

    auto computeQuats = [&](const cgltf_primitive& prim) {

        cgltf_size normalsSlot = 0;
        cgltf_size vertexCount = 0;
        const uint8_t* normalsBlob = nullptr;
        const cgltf_attribute* normalsInfo = nullptr;
        const uint8_t* tangentsBlob = nullptr;
        const cgltf_attribute* tangentsInfo = nullptr;

        for (cgltf_size slot = 0; slot < prim.attributes_count; slot++) {
            const cgltf_attribute& attr = prim.attributes[slot];
            vertexCount = attr.data->count;
            const char* uri = attr.data->buffer_view->buffer->uri;
            if (attr.type == cgltf_attribute_type_normal) {
                normalsSlot = slot;
                normalsBlob = blobs[uri] + attr.data->offset + attr.data->buffer_view->offset;
                normalsInfo = &attr;
                continue;
            }
            if (attr.type == cgltf_attribute_type_tangent) {
                tangentsBlob = blobs[uri] + attr.data->offset + attr.data->buffer_view->offset;
                tangentsInfo = &attr;
                continue;
            }
        }

        if (normalsBlob == nullptr || vertexCount == 0) {
            return;
        }

        fp16Quats.resize(vertexCount);
        printf("prideout computing %zu quats %p %p\n", vertexCount,
                normalsBlob, tangentsBlob);

        // TODO: convert (normalsBlob + normalsInfo) into "fp32Normals"
        // TODO: convert (tangentsBlob + tangentsInfo) into "fpTangents"
        // TODO: call VertexBuffer::populateTangentQuaternions with HALF4 into "fp16Quats"
        // TODO: call VertexBuffer::setBufferAt with normalsSlot
    };

    for (auto iter : nodeMap) {
        const cgltf_mesh* mesh = iter.first->mesh;
        if (mesh) {
            cgltf_size nprims = mesh->primitives_count;
            for (cgltf_size index = 0; index < nprims; ++index) {
                computeQuats(mesh->primitives[index]);
            }
        }
    }
}

} // namespace gltfio