/*
 *  Copyright 2019-2022 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  In no event and under no legal theory, whether in tort (including negligence),
 *  contract, or otherwise, unless required by applicable law (such as deliberate
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental,
 *  or consequential damages of any character arising as a result of this License or
 *  out of the use or inability to use the software (including but not limited to damages
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and
 *  all other commercial damages or losses), even if such Contributor has been advised
 *  of the possibility of such damages.
 */

#pragma once

/// \file
/// Implementation of the Diligent::DeviceObjectArchive class

// Archive file format
//
// | ArchiveHeader |
//
// | ChunkHeader | --> offset --> | NamedResourceArrayHeader |
//
// | NamedResourceArrayHeader | --> offset --> | ***DataHeader |
//
// | ***DataHeader | --> offset --> | device specific data |

#include <array>
#include <mutex>
#include <vector>
#include <unordered_map>

#include "GraphicsTypes.h"
#include "Archive.h"
#include "FileStream.h"

#include "HashUtils.hpp"
#include "RefCntAutoPtr.hpp"
#include "DynamicLinearAllocator.hpp"
#include "Serializer.hpp"

namespace Diligent
{

/// Device object archive object implementation.
class DeviceObjectArchive
{
public:
    enum class DeviceType : Uint32
    {
        OpenGL, // Same as GLES
        Direct3D11,
        Direct3D12,
        Vulkan,
        Metal_MacOS,
        Metal_iOS,
        Count
    };

    using TPRSNames = std::array<const char*, MAX_RESOURCE_SIGNATURES>;

    struct ShaderIndexArray
    {
        const Uint32* pIndices = nullptr;
        Uint32        Count    = 0;
    };

    // Serialized pipeline state auxiliary data
    struct SerializedPSOAuxData
    {
        // Shaders have been serialized without the shader reflection information.
        bool NoShaderReflection = false;
    };

    static constexpr Uint32 HeaderMagicNumber = 0xDE00000A;
    static constexpr Uint32 HeaderVersion     = 2;
    static constexpr Uint32 DataPtrAlign      = sizeof(Uint64);

    // Archive header contains the block offsets.
    // Any block can be added or removed without patching all offsets in the archive,
    // only the base offsets need to be patched.
    enum class BlockOffsetType : Uint32
    {
        // Device-specific data
        OpenGL,
        Direct3D11,
        Direct3D12,
        Vulkan,
        Metal_MacOS,
        Metal_iOS,
        Count
    };
    using TBlockBaseOffsets = std::array<Uint32, static_cast<size_t>(BlockOffsetType::Count)>;

    enum class ChunkType : Uint32
    {
        Undefined = 0,
        ArchiveDebugInfo,
        ResourceSignature,
        GraphicsPipelineStates,
        ComputePipelineStates,
        RayTracingPipelineStates,
        TilePipelineStates,
        RenderPass,
        Shaders,
        Count
    };

#define CHECK_HEADER_SIZE(Header, Size)                                                                                                           \
    static_assert(sizeof(Header) % 8 == 0, "sizeof(" #Header ") must be a multiple of 8. Use padding to align it.");                              \
    static_assert(sizeof(Header) == Size, "sizeof(" #Header ") must be " #Size ". Reading binary archive will result in invalid memory access."); \
    static_assert(sizeof(Header) % alignof(Header) == 0, "sizeof(" #Header ") is not a multiple of its alignment.");

    struct ArchiveHeader
    {
        Uint32            MagicNumber      = 0;
        Uint32            Version          = 0;
        TBlockBaseOffsets BlockBaseOffsets = {};
        Uint32            NumChunks        = 0;
        Uint32            _Padding         = ~0u;

        //ChunkHeader     Chunks  [NumChunks]
    };
    CHECK_HEADER_SIZE(ArchiveHeader, 40)

    struct ChunkHeader
    {
        ChunkHeader() noexcept {}
        ChunkHeader(ChunkType _Type) noexcept :
            Type{_Type}
        {}

        ChunkType Type     = ChunkType::Undefined;
        Uint32    Size     = 0;
        Uint32    Offset   = 0; // offset to NamedResourceArrayHeader
        Uint32    _Padding = ~0u;

        bool operator==(const ChunkHeader& Rhs) const { return Type == Rhs.Type && Size == Rhs.Size && Offset == Rhs.Offset; }
    };
    CHECK_HEADER_SIZE(ChunkHeader, 16)

    struct NamedResourceArrayHeader
    {
        Uint32 Count    = 0;
        Uint32 _Padding = ~0u;

        //Uint32 NameLength    [Count]
        //Uint32 ***DataSize   [Count]
        //Uint32 ***DataOffset [Count] // for PRSDataHeader / PSODataHeader
        //char   NameData      []
    };
    CHECK_HEADER_SIZE(NamedResourceArrayHeader, 8)

    struct DataHeaderBase
    {
        using Uint32Array = std::array<Uint32, static_cast<size_t>(DeviceType::Count)>;

        static constexpr Uint32 InvalidOffset = ~0u;

        DataHeaderBase(ChunkType _Type) noexcept :
            Type{_Type}
        {
            DeviceSpecificDataOffset.fill(Uint32{InvalidOffset});
        }

        const ChunkType Type      = ChunkType::Undefined;
        const Uint32    _Padding0 = ~0u;

        Uint32Array DeviceSpecificDataSize{};
        Uint32Array DeviceSpecificDataOffset{};

        Uint32 GetSize(DeviceType DevType) const { return DeviceSpecificDataSize[static_cast<size_t>(DevType)]; }
        Uint32 GetOffset(DeviceType DevType) const { return DeviceSpecificDataOffset[static_cast<size_t>(DevType)]; }
        Uint32 GetEndOffset(DeviceType DevType) const { return GetOffset(DevType) + GetSize(DevType); }

        void SetSize(DeviceType DevType, Uint32 Size) { DeviceSpecificDataSize[static_cast<size_t>(DevType)] = Size; }
        void SetOffset(DeviceType DevType, Uint32 Offset) { DeviceSpecificDataOffset[static_cast<size_t>(DevType)] = Offset; }
    };
    CHECK_HEADER_SIZE(DataHeaderBase, 56)

    struct PRSDataHeader : DataHeaderBase
    {
        PRSDataHeader(ChunkType _Type) noexcept :
            DataHeaderBase{_Type}
        {
            VERIFY_EXPR(Type == ChunkType::ResourceSignature);
        }
        //PipelineResourceSignatureDesc
        //PipelineResourceSignatureInternalData
    };
    CHECK_HEADER_SIZE(PRSDataHeader, 56)


    struct PSODataHeader : DataHeaderBase
    {
        PSODataHeader(ChunkType _Type) noexcept :
            DataHeaderBase{_Type}
        {
            VERIFY_EXPR((Type == ChunkType::GraphicsPipelineStates ||
                         Type == ChunkType::ComputePipelineStates ||
                         Type == ChunkType::RayTracingPipelineStates ||
                         Type == ChunkType::TilePipelineStates));
        }

        //GraphicsPipelineStateCreateInfo | ComputePipelineStateCreateInfo | TilePipelineStateCreateInfo | RayTracingPipelineStateCreateInfo
    };
    CHECK_HEADER_SIZE(PSODataHeader, 56)


    struct ShadersDataHeader : DataHeaderBase
    {
        ShadersDataHeader(ChunkType _Type = ChunkType::Shaders) noexcept :
            DataHeaderBase{_Type}
        {
            VERIFY_EXPR(Type == ChunkType::Shaders);
        }
    };
    CHECK_HEADER_SIZE(ShadersDataHeader, 56)


    struct RPDataHeader
    {
        RPDataHeader(ChunkType _Type) noexcept :
            Type{_Type}
        {
            VERIFY_EXPR(Type == ChunkType::RenderPass);
        }

        const ChunkType Type      = ChunkType::RenderPass;
        const Uint32    _Padding1 = ~0u;
    };
    CHECK_HEADER_SIZE(RPDataHeader, 8)

#undef CHECK_HEADER_SIZE


    struct ArchiveRegion
    {
        Uint32 Offset = 0;
        Uint32 Size   = 0;

        constexpr bool operator==(const ArchiveRegion& Rhs) const { return Offset == Rhs.Offset && Size == Rhs.Size; }
    };

    using NameToArchiveRegionMap = std::unordered_map<HashMapStringKey, ArchiveRegion>;

    struct ArchiveDebugInfo
    {
        String GitHash;
        Uint32 APIVersion = 0;
    };

    struct NamedResourcesMap
    {
        // TODO: use one hash map with ChunkType in the key
        NameToArchiveRegionMap Sign;
        NameToArchiveRegionMap RenderPass;
        NameToArchiveRegionMap GraphPSO;
        NameToArchiveRegionMap CompPSO;
        NameToArchiveRegionMap TilePSO;
        NameToArchiveRegionMap RayTrPSO;

        template <typename PSOCreateInfoType>
        const NameToArchiveRegionMap& GetPsoMap() const;
    };

    /// \param pArchive - Source data that this archive will be created from.
    DeviceObjectArchive(IArchive* pArchive) noexcept(false);

    const std::vector<ArchiveRegion>& GetShaderRegions(DeviceType DevType, DynamicLinearAllocator& Allocator) noexcept;

    static BlockOffsetType GetBlockOffsetType(DeviceType DevType);
    static DeviceType      RenderDeviceTypeToArchiveDeviceType(RENDER_DEVICE_TYPE Type);

    static const char* ChunkTypeToResName(ChunkType Type);

    template <typename ReourceDataType>
    bool LoadResourceData(const NameToArchiveRegionMap& NameToRegion,
                          const char*                   ResourceName,
                          ReourceDataType&              ResData) const;

    SerializedData GetDeviceSpecificData(DeviceType              DevType,
                                         const DataHeaderBase&   Header,
                                         DynamicLinearAllocator& Allocator,
                                         ChunkType               ExpectedChunkType) noexcept;

    Uint32 GetBaseOffset(BlockOffsetType Type) const
    {
        return m_BaseOffsets[static_cast<size_t>(Type)];
    }
    const std::vector<ChunkHeader>& GetChunks() const
    {
        return m_Chunks;
    }
    const NamedResourcesMap& GetResourceMap() const
    {
        return m_ResMap;
    }
    IArchive* GetArchive() const
    {
        return m_pArchive.RawPtr<IArchive>();
    }


    void RemoveDeviceData(DeviceType Dev) noexcept(false);
    void AppendDeviceData(const DeviceObjectArchive& Src, DeviceType Dev) noexcept(false);
    void Serialize(IFileStream* pStream) const noexcept(false);

    bool Validate() const;

    std::string ToString() const;

private:
    // Temporary
    friend class ArchiveRepacker;

    TBlockBaseOffsets m_BaseOffsets;
    ArchiveDebugInfo  m_DebugInfo;

    std::vector<ChunkHeader> m_Chunks;

    NamedResourcesMap m_ResMap;
    ShadersDataHeader m_ShadersHeader;

    struct ShaderRegionsInfo
    {
        std::mutex Mtx;

        std::vector<ArchiveRegion> Regions;
    };
    std::array<ShaderRegionsInfo, static_cast<size_t>(DeviceType::Count)> m_ShaderRegions;

    struct ArchiveBlock
    {
        RefCntAutoPtr<IArchive> pArchive;

        Uint32 Offset = DataHeaderBase::InvalidOffset;
        Uint32 Size   = 0;

        std::vector<Uint8> Memory; // can be used for patching

        ArchiveBlock() noexcept {}
        ArchiveBlock(const ArchiveBlock&) = default;

        ArchiveBlock(IArchive* _pArchive, Uint32 _Offset, Uint32 _Size) noexcept :
            pArchive{_pArchive}, Offset{_Offset}, Size{_Size} {}

        ArchiveBlock& operator=(ArchiveBlock&&) = default;
        ArchiveBlock& operator=(const ArchiveBlock&) = default;

        bool IsValid() const { return pArchive != nullptr && Offset != DataHeaderBase::InvalidOffset && Size != 0; }

        bool LoadToMemory();
        bool Read(Uint64 Offset, Uint64 Size, void* pData) const;
        bool Write(Uint64 Offset, Uint64 Size, const void* pData);
    };
    using DeviceSpecificBlocks = std::array<ArchiveBlock, static_cast<size_t>(BlockOffsetType::Count)>;

    ArchiveBlock         m_CommonData;
    DeviceSpecificBlocks m_DeviceSpecific;

    RefCntAutoPtr<IArchive> m_pArchive; // archive is thread-safe
};


template <typename ReourceDataType>
bool DeviceObjectArchive::LoadResourceData(const NameToArchiveRegionMap& NameToRegion,
                                           const char*                   ResourceName,
                                           ReourceDataType&              ResData) const
{
    auto it = NameToRegion.find(ResourceName);
    if (it == NameToRegion.end())
    {
        LOG_ERROR_MESSAGE("Resource '", ResourceName, "' is not present in the archive");
        return false;
    }
    VERIFY_EXPR(SafeStrEqual(ResourceName, it->first.GetStr()));
    // Use string copy from the map
    ResourceName = it->first.GetStr();

    const auto& Region = it->second;
    void*       pData  = ResData.Allocator.Allocate(Region.Size, DataPtrAlign);
    if (!m_pArchive.RawPtr<IArchive>()->Read(Region.Offset, Region.Size, pData))
    {
        LOG_ERROR_MESSAGE("Failed to read resource '", ResourceName, "' data from the archive");
        return false;
    }

    Serializer<SerializerMode::Read> Ser{SerializedData{pData, Region.Size}};

    using HeaderType = typename std::remove_reference<decltype(*ResData.pHeader)>::type;
    ResData.pHeader  = Ser.Cast<HeaderType>();
    if (ResData.pHeader->Type != ResData.ExpectedChunkType)
    {
        LOG_ERROR_MESSAGE("Invalid chunk header: '", ChunkTypeToResName(ResData.pHeader->Type),
                          "'; expected: '", ChunkTypeToResName(ResData.ExpectedChunkType), "'.");
        return false;
    }

    auto Res = ResData.Deserialize(ResourceName, Ser);
    VERIFY_EXPR(Ser.IsEnded());
    return Res;
}

} // namespace Diligent