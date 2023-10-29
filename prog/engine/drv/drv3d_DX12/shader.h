#pragma once

#include <EASTL/array.h>
#include <EASTL/sort.h>
#include <EASTL/variant.h>
#include <EASTL/unordered_set.h>
#include <osApiWrappers/dag_critSec.h>
#include <osApiWrappers/dag_rwLock.h>
#include <util/dag_hashedKeyMap.h>
#include <dxil/compiled_shader_header.h>
#include <dxil/utility.h>
#include <osApiWrappers/dag_atomic.h>
#include <shadersBinaryData.h>

static constexpr uint32_t MAX_VERTEX_ATTRIBUTES = 16;
static constexpr uint32_t MAX_VERTEX_INPUT_STREAMS = 4;
static constexpr uint32_t MAX_SEMANTIC_INDEX = VSDR_TEXC14 + 1;

namespace drv3d_dx12
{
class DeviceContext;
class Image;

class Device;
struct InputLayout
{
  struct VertexAttributesDesc
  {
    // We use 25 unique locations, each one bit
    uint32_t locationMask = 0;
    // for each location we use two corresponding bits to store index into streams
    uint64_t locationSourceStream = 0;
    // format uses top 5 bits and the remaining 11 bits store the offset as dword count (8k bytes of
    // offset max)
    uint16_t compactedFormatAndOffset[MAX_SEMANTIC_INDEX]{};

    static constexpr uint32_t vsdt_format_shift = 16;
    static constexpr uint32_t vsdt_max_value = VSDT_UINT4 >> vsdt_format_shift;
    static constexpr uint32_t format_index_bits = BitsNeeded<vsdt_max_value>::VALUE;
    static constexpr uint32_t offset_bits = sizeof(uint16_t) * 8 - format_index_bits;
    static constexpr uint16_t offset_mask = (1u << offset_bits) - 1;
    static constexpr uint16_t format_index_mask = ~offset_mask;
    static constexpr uint32_t validate_format_index_mask = ((1u << format_index_bits) - 1) << vsdt_format_shift;

    void useLocation(uint32_t index)
    {
      G_ASSERT(index < MAX_SEMANTIC_INDEX);
      locationMask |= 1u << index;
    }
    void setLocationStreamSource(uint32_t location, uint32_t stream)
    {
      G_ASSERT(stream < MAX_VERTEX_INPUT_STREAMS);
      G_STATIC_ASSERT(MAX_VERTEX_INPUT_STREAMS - 1 <= 3); // make sure we only need 2 bits for
                                                          // stream index
      locationSourceStream |= static_cast<uint64_t>(stream & 3) << (location * 2);
    }
    void setLocationStreamOffset(uint32_t location, uint32_t offset)
    {
      G_ASSERT(0 == (offset % 4));
      G_ASSERT((offset / 4) == ((offset / 4) & offset_mask));
      compactedFormatAndOffset[location] = (compactedFormatAndOffset[location] & format_index_mask) | ((offset / 4) & offset_mask);
    }
    void setLocationFormatIndex(uint32_t location, uint32_t index)
    {
      G_ASSERT(index == (index & validate_format_index_mask));
      compactedFormatAndOffset[location] =
        (compactedFormatAndOffset[location] & offset_mask) | ((index >> vsdt_format_shift) << offset_bits);
    }

    constexpr bool usesLocation(uint32_t index) const { return 0 != (1 & (locationMask >> index)); }
    constexpr uint32_t getLocationStreamSource(uint32_t location) const
    {
      return static_cast<uint32_t>(locationSourceStream >> (location * 2)) & 3;
    }
    constexpr uint32_t getLocationStreamOffset(uint32_t location) const
    {
      return static_cast<uint32_t>(compactedFormatAndOffset[location] & offset_mask) * 4;
    }
    constexpr uint32_t getLocationFormatIndex(uint32_t location) const
    {
      return (compactedFormatAndOffset[location] >> offset_bits) << vsdt_format_shift;
    }
    /*constexpr*/ DXGI_FORMAT getLocationFormat(uint32_t location) const
    {
      switch (((compactedFormatAndOffset[location] & format_index_mask) >> offset_bits) << vsdt_format_shift)
      {
        case VSDT_FLOAT1: return DXGI_FORMAT_R32_FLOAT;
        case VSDT_FLOAT2: return DXGI_FORMAT_R32G32_FLOAT;
        case VSDT_FLOAT3: return DXGI_FORMAT_R32G32B32_FLOAT;
        case VSDT_FLOAT4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case VSDT_INT1: return DXGI_FORMAT_R32_SINT;
        case VSDT_INT2: return DXGI_FORMAT_R32G32_SINT;
        case VSDT_INT3: return DXGI_FORMAT_R32G32B32_SINT;
        case VSDT_INT4: return DXGI_FORMAT_R32G32B32A32_SINT;
        case VSDT_UINT1: return DXGI_FORMAT_R32_UINT;
        case VSDT_UINT2: return DXGI_FORMAT_R32G32_UINT;
        case VSDT_UINT3: return DXGI_FORMAT_R32G32B32_UINT;
        case VSDT_UINT4: return DXGI_FORMAT_R32G32B32A32_UINT;
        case VSDT_HALF2: return DXGI_FORMAT_R16G16_FLOAT;
        case VSDT_SHORT2N: return DXGI_FORMAT_R16G16_SNORM;
        case VSDT_SHORT2: return DXGI_FORMAT_R16G16_SINT;
        case VSDT_USHORT2N: return DXGI_FORMAT_R16G16_UNORM;

        case VSDT_HALF4: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case VSDT_SHORT4N: return DXGI_FORMAT_R16G16B16A16_SNORM;
        case VSDT_SHORT4: return DXGI_FORMAT_R16G16B16A16_SINT;
        case VSDT_USHORT4N: return DXGI_FORMAT_R16G16B16A16_UNORM;

        case VSDT_UDEC3: return DXGI_FORMAT_R10G10B10A2_UINT;
        case VSDT_DEC3N: return DXGI_FORMAT_R10G10B10A2_UNORM;

        case VSDT_E3DCOLOR: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case VSDT_UBYTE4: return DXGI_FORMAT_R8G8B8A8_UINT;
        default: G_ASSERTF(false, "invalid vertex declaration type"); break;
      }
      return DXGI_FORMAT_UNKNOWN;
    }
  };

  struct VertexStreamsDesc
  {
    uint8_t usageMask : MAX_VERTEX_INPUT_STREAMS;
    uint8_t stepRateMask : MAX_VERTEX_INPUT_STREAMS;
    constexpr VertexStreamsDesc() : usageMask{0}, stepRateMask{0} {}

    void useStream(uint32_t index)
    {
      G_ASSERT(index < MAX_VERTEX_INPUT_STREAMS);
      usageMask |= 1u << index;
    }
    void setStreamStepRate(uint32_t index, D3D12_INPUT_CLASSIFICATION rate)
    {
      G_ASSERT(static_cast<uint32_t>(rate) <= 1); // -V547
      stepRateMask |= static_cast<uint32_t>(rate) << index;
    }

    constexpr bool usesStream(uint32_t index) const { return 0 != (1 & (usageMask >> index)); }
    constexpr D3D12_INPUT_CLASSIFICATION getStreamStepRate(uint32_t index) const
    {
      return static_cast<D3D12_INPUT_CLASSIFICATION>(1 & (stepRateMask >> index));
    }
  };

  template <typename T>
  void generateInputElementDescriptions(T clb) const
  {
    auto attirbMask = vertexAttributeSet.locationMask;
    for (uint32_t i = 0; i < MAX_SEMANTIC_INDEX && attirbMask; ++i, attirbMask >>= 1)
    {
      if (0 == (1 & attirbMask))
        continue;

      auto semanticInfo = dxil::getSemanticInfoFromIndex(i);
      G_ASSERT(semanticInfo);
      if (!semanticInfo)
        break;

      D3D12_INPUT_ELEMENT_DESC desc;
      desc.SemanticName = semanticInfo->name;
      desc.SemanticIndex = semanticInfo->index;
      desc.Format = vertexAttributeSet.getLocationFormat(i);
      desc.InputSlot = vertexAttributeSet.getLocationStreamSource(i);
      desc.AlignedByteOffset = vertexAttributeSet.getLocationStreamOffset(i);
      desc.InputSlotClass = inputStreamSet.getStreamStepRate(desc.InputSlot);
      desc.InstanceDataStepRate = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA == desc.InputSlotClass ? 0 : 1;
      clb(desc);
    }
  }

  constexpr bool validateAttributeUse(uint32_t expected) const { return expected == (vertexAttributeSet.locationMask & expected); }

  constexpr bool matchesAttributeUse(uint32_t expected) const { return vertexAttributeSet.locationMask == expected; }

  // This creates a copy of this with all attributes removed that are not
  // part of the used mask, it also removes any input stream that might no
  // longer be needed.
  InputLayout getLayoutForAttributeUse(uint32_t used) const
  {
    InputLayout other;
    uint32_t aMask = vertexAttributeSet.locationMask & used;
    for (uint32_t i = 0; i < MAX_SEMANTIC_INDEX && aMask; ++i, aMask >>= 1)
    {
      if (0 == (1 & aMask))
        continue;
      other.vertexAttributeSet.useLocation(i);
      auto streamIndex = vertexAttributeSet.getLocationStreamSource(i);
      other.vertexAttributeSet.setLocationStreamSource(i, streamIndex);
      other.inputStreamSet.useStream(streamIndex);
      other.vertexAttributeSet.setLocationStreamOffset(i, vertexAttributeSet.getLocationStreamOffset(i));
      other.vertexAttributeSet.setLocationFormatIndex(i, vertexAttributeSet.getLocationFormatIndex(i));
    }
    uint32_t sMask = other.inputStreamSet.usageMask;
    for (uint32_t i = 0; i < MAX_VERTEX_INPUT_STREAMS && sMask; ++i, sMask >>= 1)
    {
      if (0 == (1 & sMask))
        continue;

      other.inputStreamSet.setStreamStepRate(i, inputStreamSet.getStreamStepRate(i));
    }

    return other;
  }

  VertexAttributesDesc vertexAttributeSet;
  VertexStreamsDesc inputStreamSet;

  void fromVdecl(const VSDTYPE *decl);
};

inline bool operator==(const InputLayout::VertexAttributesDesc &l, const InputLayout::VertexAttributesDesc &r)
{
  return l.locationMask == r.locationMask && l.locationSourceStream == r.locationSourceStream &&
         eastl::equal(eastl::begin(l.compactedFormatAndOffset), eastl::end(l.compactedFormatAndOffset),
           eastl::begin(r.compactedFormatAndOffset));
}

inline bool operator!=(const InputLayout::VertexAttributesDesc &l, const InputLayout::VertexAttributesDesc &r) { return !(l == r); }

inline bool operator==(const InputLayout::VertexStreamsDesc &l, const InputLayout::VertexStreamsDesc &r)
{
  return l.usageMask == r.usageMask && l.stepRateMask == r.stepRateMask;
}

inline bool operator!=(const InputLayout::VertexStreamsDesc &l, const InputLayout::VertexStreamsDesc &r) { return !(l == r); }

inline bool operator==(const InputLayout &l, const InputLayout &r)
{
  return l.inputStreamSet == r.inputStreamSet && l.vertexAttributeSet == r.vertexAttributeSet;
}

inline bool operator!=(const InputLayout &l, const InputLayout &r) { return !(l == r); }

struct ShaderIdentifier
{
  dxil::HashValue shaderHash;
  uint32_t shaderSize;
};

struct StageShaderModuleHeader
{
  ShaderIdentifier ident = {};
  dxil::ShaderHeader header = {};
  eastl::string debugName;
};

struct StageShaderModule : StageShaderModuleHeader
{
  eastl::unique_ptr<uint8_t[]> byteCode;
  size_t byteCodeSize = 0;

  explicit operator bool() const { return 0 != byteCodeSize; }
};

struct VertexShaderModule : StageShaderModule
{
  VertexShaderModule() = default;
  ~VertexShaderModule() = default;

  VertexShaderModule(VertexShaderModule &&) = default;
  VertexShaderModule &operator=(VertexShaderModule &&) = default;

  VertexShaderModule(StageShaderModule &&module) : StageShaderModule{eastl::move(module)} {}
  eastl::unique_ptr<StageShaderModule> geometryShader;
  eastl::unique_ptr<StageShaderModule> hullShader;
  eastl::unique_ptr<StageShaderModule> domainShader;
};

#if _TARGET_XBOXONE
// XB1 has no mesh shader stage
inline constexpr bool is_mesh(const VertexShaderModule &) { return false; }
#else
inline bool is_mesh(const VertexShaderModule &module)
{
  return dxil::ShaderStage::MESH == static_cast<dxil::ShaderStage>(module.header.shaderType);
}
#endif

using PixelShaderModule = StageShaderModule;
using ComputeShaderModule = StageShaderModule;

#if D3D_HAS_RAY_TRACING
struct RaytraceProgram
{
  eastl::unique_ptr<ShaderID[]> shaders;
  eastl::unique_ptr<RaytraceShaderGroup[]> shaderGroups;
  uint32_t groupCount = 0;
  uint32_t shaderCount = 0;
  uint32_t maxRecursionDepth = 0;
};
using RaytraceShaderModule = StageShaderModule;
#endif

struct ShaderStageResouceUsageMask
{
  eastl::bitset<dxil::MAX_B_REGISTERS> bRegisterMask;
  eastl::bitset<dxil::MAX_T_REGISTERS> tRegisterMask;
  eastl::bitset<dxil::MAX_S_REGISTERS> sRegisterMask;
  eastl::bitset<dxil::MAX_U_REGISTERS> uRegisterMask;

  ShaderStageResouceUsageMask() = default;
  ShaderStageResouceUsageMask(const ShaderStageResouceUsageMask &) = default;
  // broadcast a single value to all masks
  explicit ShaderStageResouceUsageMask(uint32_t v) : bRegisterMask{v}, tRegisterMask{v}, sRegisterMask{v}, uRegisterMask{v} {}
  explicit ShaderStageResouceUsageMask(const dxil::ShaderHeader &header) :
    bRegisterMask{header.resourceUsageTable.bRegisterUseMask},
    tRegisterMask{header.resourceUsageTable.tRegisterUseMask},
    sRegisterMask{header.resourceUsageTable.sRegisterUseMask},
    uRegisterMask{header.resourceUsageTable.uRegisterUseMask}
  {}

  ShaderStageResouceUsageMask &operator|=(const ShaderStageResouceUsageMask &other)
  {
    bRegisterMask |= other.bRegisterMask;
    tRegisterMask |= other.tRegisterMask;
    sRegisterMask |= other.sRegisterMask;
    uRegisterMask |= other.uRegisterMask;
    return *this;
  }

  ShaderStageResouceUsageMask operator&(const ShaderStageResouceUsageMask &other) const
  {
    ShaderStageResouceUsageMask cpy{*this};
    cpy.bRegisterMask &= other.bRegisterMask;
    cpy.tRegisterMask &= other.tRegisterMask;
    cpy.sRegisterMask &= other.sRegisterMask;
    cpy.uRegisterMask &= other.uRegisterMask;
    return cpy;
  }

  ShaderStageResouceUsageMask &operator^=(const ShaderStageResouceUsageMask &other)
  {
    bRegisterMask ^= other.bRegisterMask;
    tRegisterMask ^= other.tRegisterMask;
    sRegisterMask ^= other.sRegisterMask;
    uRegisterMask ^= other.uRegisterMask;
    return *this;
  }
};

struct GraphicsProgramUsageInfo
{
  InputLayoutID input = InputLayoutID::Null();
  GraphicsProgramID programId = GraphicsProgramID::Null();
};

class IdManager
{
  uint32_t lastAllocatedId = 0;
  eastl::vector<uint32_t> freeIds;

public:
  uint32_t allocate()
  {
    uint32_t id;
    if (!freeIds.empty())
    {
      id = freeIds.back();
      freeIds.pop_back();
    }
    else
    {
      id = ++lastAllocatedId;
    }
    return id;
  }
  void free(uint32_t id)
  {
    G_ASSERT(id <= lastAllocatedId);
    if (id == lastAllocatedId)
    {
      --lastAllocatedId;

      // try to tidy up the free id list
      auto at = eastl::find(begin(freeIds), end(freeIds), lastAllocatedId);
      while (at != end(freeIds))
      {
        freeIds.erase(at);
        at = eastl::find(begin(freeIds), end(freeIds), --lastAllocatedId);
      }
    }
    else
    {
      freeIds.push_back(id);
    }
  }
  void clear()
  {
    lastAllocatedId = 0;
    freeIds.clear();
  }

  template <typename T>
  void iterateAllocated(T t)
  {
    // to be reasonable fast, we iterate between free slots until we reach last allocated id
    eastl::sort(begin(freeIds), end(freeIds));
    uint32_t at = 1;
    for (auto se : freeIds)
    {
      for (; at < se; ++at)
        t(at);

      at = se + 1;
    }
    for (; at <= lastAllocatedId; ++at)
      t(at);
  }
};

template <typename T>
inline void ensure_container_space(T &v, size_t space)
{
  if (v.size() < space)
    v.resize(space);
}
template <typename T, typename U>
inline void ensure_container_space(T &v, size_t space, U &&u)
{
  if (v.size() < space)
    v.resize(space, u);
}

template <typename C, typename V>
inline void set_at(C &c, size_t i, V v)
{
  if (c.size() == i)
  {
    c.push_back(eastl::move(v));
  }
  else
  {
    ensure_container_space(c, i + 1);
    c[i] = eastl::move(v);
  }
}

struct GraphicsProgramTemplate
{
  ShaderID vertexShader;
  ShaderID pixelShader;
};

struct GraphicsProgramInstance
{
  InputLayoutID inputLayout = InputLayoutID::Null();
  GraphicsProgramID internalProgram = GraphicsProgramID::Null();
  int refCount = 0;
  void addRef() { interlocked_increment(refCount); }
  void deleteAllReferences()
  {
    refCount = 0;
    internalProgram = GraphicsProgramID::Null();
  }
  int getRefCount() { return interlocked_acquire_load(refCount); }
  bool delRef()
  {
    if (interlocked_decrement(refCount) != 0)
      return false;
    internalProgram = GraphicsProgramID::Null();
    return true;
  }
};

template <typename T>
void inspect_scripted_shader_bin_dump(ScriptedShadersBinDumpOwner *dump, T inspector)
{
  auto v2 = dump->getDumpV2();
  if (!v2)
  {
    return;
  }
  inspector.vertexShaderCount(v2->vprId.size());
  inspector.pixelOrComputeShaderCount(v2->fshId.size());
  for (auto &cls : v2->classes)
  {
    for (auto &prog : cls.shrefStorage)
    {
      if (prog.vprId != 0xFFFF)
      {
        if (prog.fshId != 0xFFFF)
        {
          inspector.addGraphicsProgram(prog.vprId, prog.fshId);
        }
        else
        {
          inspector.addGraphicsProgramWithNullPixelShader(prog.vprId);
        }
      }
      else
      {
        inspector.addComputeProgram(prog.fshId);
      }
    }
  }
}

class ShaderProgramGroup
{
  friend class ShaderProgramGroups;
  ScriptedShadersBinDumpOwner *dump = nullptr;
  eastl::vector<eastl::variant<eastl::monostate, ShaderID, ProgramID>> pixelShaderComputeProgramIDMap;
  eastl::vector<GraphicsProgramTemplate> graphicsProgramTemplates;
  uint32_t vertexShaderCount = 0;
  uint32_t pixelShaderCount = 0;
  uint32_t computeShaderCount = 0;

  struct ScriptedShaderBinDumpInspector
  {
    uint32_t groupID;
    ShaderID nullPixelShader;
    ShaderProgramGroup *target;
    void vertexShaderCount(uint32_t count) { target->vertexShaderCount = count; }
    void pixelOrComputeShaderCount(uint32_t count) { target->pixelShaderComputeProgramIDMap.resize(count); }
    void addGraphicsProgram(uint16_t vs_index, uint16_t ps_index)
    {
      ShaderID psID;
      if (auto shader = eastl::get_if<ShaderID>(&target->pixelShaderComputeProgramIDMap[ps_index]))
      {
        psID = *shader;
      }
      else
      {
        psID = ShaderID::make(groupID, target->pixelShaderCount++);
        target->pixelShaderComputeProgramIDMap[ps_index] = psID;
      }
      auto vsID = ShaderID::make(groupID, vs_index);
      auto ref = eastl::find_if(begin(target->graphicsProgramTemplates), end(target->graphicsProgramTemplates),
        [vsID, psID](auto &prog) { return vsID == prog.vertexShader && psID == prog.pixelShader; });
      if (end(target->graphicsProgramTemplates) == ref)
      {
        GraphicsProgramTemplate prog{vsID, psID};
        target->graphicsProgramTemplates.push_back(prog);
      }
    }
    void addGraphicsProgramWithNullPixelShader(uint16_t vs_index)
    {
      auto psID = nullPixelShader;
      auto vsID = ShaderID::make(groupID, vs_index);
      auto ref = eastl::find_if(begin(target->graphicsProgramTemplates), end(target->graphicsProgramTemplates),
        [vsID, psID](auto &prog) { return vsID == prog.vertexShader && psID == prog.pixelShader; });
      if (end(target->graphicsProgramTemplates) == ref)
      {
        GraphicsProgramTemplate prog{vsID, psID};
        target->graphicsProgramTemplates.push_back(prog);
      }
    }
    void addComputeProgram(uint32_t shader_index)
    {
      if (!eastl::holds_alternative<ProgramID>(target->pixelShaderComputeProgramIDMap[shader_index]))
      {
        target->pixelShaderComputeProgramIDMap[shader_index] = ProgramID::asComputeProgram(groupID, target->computeShaderCount++);
      }
    }
  };

protected:
  void clear()
  {
    dump = nullptr;
    pixelShaderComputeProgramIDMap.clear();
    graphicsProgramTemplates.clear();
    vertexShaderCount = 0;
  }

  void setup(const ShaderProgramGroup *base, ScriptedShadersBinDumpOwner *d, ShaderID null_pixel_shader)
  {
    dump = d;
    inspect_scripted_shader_bin_dump(d, ScriptedShaderBinDumpInspector{getGroupID(base), null_pixel_shader, this});
    graphicsProgramTemplates.shrink_to_fit();
    debug("DX12: Shader bindump %p contains %u vertex shaders, %u pixel shaders, %u graphics programs, %u compute programs", d,
      vertexShaderCount, pixelShaderCount, graphicsProgramTemplates.size(), computeShaderCount);
  }

  bool isEmpty() const { return nullptr == dump; }

  bool isAssociatedWith(const ScriptedShadersBinDumpOwner *d) const { return dump == d; }

  void trim()
  {
    pixelShaderComputeProgramIDMap.shrink_to_fit();
    graphicsProgramTemplates.shrink_to_fit();
  }

  uint32_t getGroupID(const ShaderProgramGroup *base) const { return 1 + (this - base); }

  template <typename T>
  void iterateComputeShaders(T clb) const
  {
    for (const auto &e : pixelShaderComputeProgramIDMap)
    {
      if (eastl::holds_alternative<ProgramID>(e))
      {
        clb(eastl::get<ProgramID>(e));
      }
    }
  }

  template <typename T>
  void iteratePixelShaders(T clb) const
  {
    for (const auto &e : pixelShaderComputeProgramIDMap)
    {
      if (eastl::holds_alternative<ShaderID>(e))
      {
        clb(eastl::get<ShaderID>(e));
      }
    }
  }

  GraphicsProgramID findGraphicsProgram(const ShaderProgramGroup *base, ShaderID vs, ShaderID ps) const
  {
    auto ref = eastl::find_if(begin(graphicsProgramTemplates), end(graphicsProgramTemplates),
      [vs, ps](const auto &prog) { return vs == prog.vertexShader && ps == prog.pixelShader; });
    if (ref != end(graphicsProgramTemplates))
    {
      return GraphicsProgramID::make(getGroupID(base), ref - begin(graphicsProgramTemplates));
    }
    return GraphicsProgramID::Null();
  }

  GraphicsProgramID addGraphicsProgram(const ShaderProgramGroup *base, ShaderID vs, ShaderID ps)
  {
    GraphicsProgramID id;
    auto ref = eastl::find_if(begin(graphicsProgramTemplates), end(graphicsProgramTemplates),
      [](const auto &prog) { return ShaderID::Null() == prog.vertexShader; });
    if (ref == end(graphicsProgramTemplates))
    {
      id = GraphicsProgramID::make(getGroupID(base), graphicsProgramTemplates.size());
      graphicsProgramTemplates.push_back(GraphicsProgramTemplate{vs, ps});
    }
    else
    {
      id = GraphicsProgramID::make(getGroupID(base), ref - begin(graphicsProgramTemplates));
      ref->vertexShader = vs;
      ref->pixelShader = ps;
    }
    return id;
  }

  GraphicsProgramTemplate getShadersOfGraphicsProgram(GraphicsProgramID gpid) const
  {
    return graphicsProgramTemplates[gpid.getIndex()];
  }

  template <typename T>
  void iterateGraphicsPrograms(const ShaderProgramGroup *base, T clb)
  {
    auto gid = getGroupID(base);
    for (size_t i = 0; i < graphicsProgramTemplates.size(); ++i)
    {
      clb(GraphicsProgramID::make(gid, i));
    }
  }

  template <typename T>
  void removeGraphicsProgramWithMatchingVertexShader(const ShaderProgramGroup *base, ShaderID vs, T clb)
  {
    auto gid = getGroupID(base);
    for (size_t i = 0; i < graphicsProgramTemplates.size(); ++i)
    {
      auto &gp = graphicsProgramTemplates[i];
      if (gp.vertexShader == vs)
      {
        clb(GraphicsProgramID::make(gid, i));
        gp.vertexShader = ShaderID::Null();
        gp.pixelShader = ShaderID::Null();
      }
    }
  }

  template <typename T>
  void removeGraphicsProgramWithMatchingPixelShader(const ShaderProgramGroup *base, ShaderID ps, T clb)
  {
    auto gid = getGroupID(base);
    for (size_t i = 0; i < graphicsProgramTemplates.size(); ++i)
    {
      auto &gp = graphicsProgramTemplates[i];
      if (gp.pixelShader == ps)
      {
        clb(GraphicsProgramID::make(gid, i));
        gp.vertexShader = ShaderID::Null();
        gp.pixelShader = ShaderID::Null();
      }
    }
  }

  dxil::ShaderStage shaderFromIndex(const ShaderProgramGroup *base, uint32_t shader_index, ShaderCodeType type, void *ident)
  {
    auto groupID = getGroupID(base);
    if (ShaderCodeType::VERTEX == type)
    {
      G_ASSERT_RETURN(shader_index < vertexShaderCount, dxil::ShaderStage::VERTEX);
      *static_cast<VPROG *>(ident) = ShaderID::make(groupID, shader_index).exportValue();
      return dxil::ShaderStage::VERTEX;
    }
    G_ASSERT_RETURN(shader_index < pixelShaderComputeProgramIDMap.size(), dxil::ShaderStage::VERTEX);
    if (auto shaderID = eastl::get_if<ShaderID>(&pixelShaderComputeProgramIDMap[shader_index]))
    {
      *static_cast<FSHADER *>(ident) = shaderID->exportValue();
      return dxil::ShaderStage::PIXEL;
    }
    if (auto programID = eastl::get_if<ProgramID>(&pixelShaderComputeProgramIDMap[shader_index]))
    {
      *static_cast<FSHADER *>(ident) = programID->exportValue();
      return dxil::ShaderStage::COMPUTE;
    }
    return dxil::ShaderStage::VERTEX;
  }
};

class ShaderProgramGroups
{
  // shaderGroup 0 is always to "global" shaderGroup and is not handled by groups
  eastl::array<ShaderProgramGroup, max_scripted_shaders_bin_groups - 1> groups{};
  // For shaders we only track used ids. When new shaders are created or
  // shaders are deleted we just manage the ids and hand over ownership
  // to the device context backend with the appropriate commands.
  IdManager vertexShaderIds;
  IdManager pixelShaderIds;
  IdManager computeIds;

  typedef uint64_t hash_key_t;
  HashedKeyMap<hash_key_t, uint32_t, hash_key_t(0), oa_hashmap_util::MumStepHash<hash_key_t>> graphicsProgramsHashMap;

  eastl::vector<GraphicsProgramTemplate> graphicsProgramTemplates;
  eastl::vector<GraphicsProgramInstance> graphicsProgramInstances;
  uint32_t graphicsProgramsHashMapDead = 0;

  ShaderProgramGroup *getShaderGroupForShader(ShaderID id)
  {
    if (auto index = id.getGroup())
    {
      G_ASSERT(index > 0 && (index - 1) < groups.size());
      return &groups[index - 1];
    }
    return nullptr;
  }

  const ShaderProgramGroup *getShaderGroupForShader(ShaderID id) const
  {
    if (auto index = id.getGroup())
    {
      G_ASSERT(index > 0 && (index - 1) < groups.size());
      return &groups[index - 1];
    }
    return nullptr;
  }

  ShaderProgramGroup *getShaderGroupForShaders(ShaderID vs, ShaderID ps)
  {
    if (auto index = vs.getGroup())
    {
      G_ASSERT(index > 0 && (index - 1) < groups.size());
      if (0 == ps.getGroup() || ps.getGroup() == index)
      {
        return &groups[index - 1];
      }
    }
    return nullptr;
  }

  const ShaderProgramGroup *getShaderGroupForShaders(ShaderID vs, ShaderID ps) const
  {
    if (auto index = vs.getGroup())
    {
      G_ASSERT(index > 0 && (index - 1) < groups.size());
      if (0 == ps.getGroup() || ps.getGroup() == index)
      {
        return &groups[index - 1];
      }
    }
    return nullptr;
  }

  ShaderProgramGroup *getShaderGroupForProgram(ProgramID id)
  {
    if (auto index = id.getGroup())
    {
      G_ASSERT(index > 0 && (index - 1) < groups.size());
      return &groups[index - 1];
    }
    return nullptr;
  }

  const ShaderProgramGroup *getShaderGroupForProgram(ProgramID id) const
  {
    if (auto index = id.getGroup())
    {
      G_ASSERT(index > 0 && (index - 1) < groups.size());
      return &groups[index - 1];
    }
    return nullptr;
  }

  ShaderProgramGroup *getShaderGroupForGraphicsProgram(GraphicsProgramID id)
  {
    if (auto index = id.getGroup())
    {
      G_ASSERT(index > 0 && (index - 1) < groups.size());
      return &groups[index - 1];
    }
    return nullptr;
  }

  const ShaderProgramGroup *getShaderGroupForGraphicsProgram(GraphicsProgramID id) const
  {
    if (auto index = id.getGroup())
    {
      G_ASSERT(index > 0 && (index - 1) < groups.size());
      return &groups[index - 1];
    }
    return nullptr;
  }

public:
  void trim(ShaderProgramGroup *shaderGroup) { shaderGroup->trim(); }
  void removePixelShader(ShaderID shader) { pixelShaderIds.free(shader.getIndex()); }
  void removeVertexShader(ShaderID shader)
  {
    G_ASSERTF(0 == shader.getGroup(), "Invalid to call this function with a shader with a shaderGroup id "
                                      "other than 0");
    vertexShaderIds.free(shader.getIndex());
  }
  void removeComputeProgram(ProgramID program) { computeIds.free(program.getIndex()); }
  template <typename T>
  void iterateAllPixelShaders(T clb)
  {
    for (auto &g : groups)
    {
      g.iteratePixelShaders(clb);
    }
    pixelShaderIds.iterateAllocated([&](uint32_t id) { clb(ShaderID::make(0, id)); });
  }
  template <typename T>
  void itarateAllVertexShaders(T clb)
  {
    vertexShaderIds.iterateAllocated([&](uint32_t id) { clb(ShaderID::make(0, id)); });
  }
  template <typename T>
  void iterateAllComputeShaders(T clb)
  {
    for (auto &g : groups)
    {
      g.iterateComputeShaders(clb);
    }
    computeIds.iterateAllocated([&](uint32_t id) { clb(ProgramID::asComputeProgram(0, id)); });
  }
  ProgramID addComputeShaderProgram()
  {
    auto program = ProgramID::asComputeProgram(0, computeIds.allocate());
    return program;
  }
  ShaderID addPixelShader()
  {
    auto id = ShaderID::make(0, pixelShaderIds.allocate());
    return id;
  }
  ShaderID addVertexShader()
  {
    auto id = ShaderID::make(0, vertexShaderIds.allocate());
    return id;
  }
  void reset()
  {
    for (auto &shaderGroup : groups)
      shaderGroup.clear();
    vertexShaderIds.clear();
    pixelShaderIds.clear();
    computeIds.clear();
    graphicsProgramTemplates.clear();
    clearGraphicsProgramsInstances();
  }

  GraphicsProgramID findGraphicsProgram(ShaderID vs, ShaderID ps) const
  {
    if (auto shaderGroup = getShaderGroupForShaders(vs, ps))
    {
      return shaderGroup->findGraphicsProgram(groups.data(), vs, ps);
    }
    auto ref = eastl::find_if(begin(graphicsProgramTemplates), end(graphicsProgramTemplates),
      [vs, ps](const auto &pair) { return vs == pair.vertexShader && ps == pair.pixelShader; });
    if (ref == end(graphicsProgramTemplates))
    {
      return GraphicsProgramID::Null();
    }
    else
    {
      return GraphicsProgramID::make(0, ref - begin(graphicsProgramTemplates));
    }
  }

  GraphicsProgramID addGraphicsProgram(ShaderID vs, ShaderID ps)
  {
    if (auto shaderGroup = getShaderGroupForShaders(vs, ps))
    {
      return shaderGroup->addGraphicsProgram(groups.data(), vs, ps);
    }
    GraphicsProgramID id;
    auto ref = eastl::find_if(begin(graphicsProgramTemplates), end(graphicsProgramTemplates),
      [](const auto &pair) { return ShaderID::Null() == pair.vertexShader; });
    if (ref == end(graphicsProgramTemplates))
    {
      id = GraphicsProgramID::make(0, graphicsProgramTemplates.size());
      graphicsProgramTemplates.push_back(GraphicsProgramTemplate{vs, ps});
    }
    else
    {
      id = GraphicsProgramID::make(0, ref - begin(graphicsProgramTemplates));
      ref->vertexShader = vs;
      ref->pixelShader = ps;
    }
    return id;
  }

  template <typename T>
  void iterateAllGraphicsPrograms(T clb)
  {
    for (auto &shaderGroup : groups)
    {
      shaderGroup.iterateGraphicsPrograms(groups.data(), clb);
    }
    for (size_t i = 0; i < graphicsProgramTemplates.size(); ++i)
    {
      if (ShaderID::Null() != graphicsProgramTemplates[i].vertexShader)
      {
        clb(GraphicsProgramID::make(0, i));
      }
    }
  }

  template <typename T>
  void removeGraphicsProgramWithMatchingVertexShader(ShaderID vs, T clb)
  {
    if (auto shaderGroup = getShaderGroupForShader(vs))
    {
      shaderGroup->removeGraphicsProgramWithMatchingVertexShader(groups.data(), vs, clb);
    }

    // should a vertex shader with shaderGroup 1 be used with a pixel shader of shaderGroup 2
    // it will end up be in the global shaderGroup
    for (size_t i = 0; i < graphicsProgramTemplates.size(); ++i)
    {
      if (vs == graphicsProgramTemplates[i].vertexShader)
      {
        clb(GraphicsProgramID::make(0, i));
        graphicsProgramTemplates[i].vertexShader = ShaderID::Null();
        graphicsProgramTemplates[i].pixelShader = ShaderID::Null();
      }
    }
  }

  template <typename T>
  void removeGraphicsProgramWithMatchingPixelShader(ShaderID ps, T clb)
  {
    if (auto shaderGroup = getShaderGroupForShader(ps))
    {
      shaderGroup->removeGraphicsProgramWithMatchingPixelShader(groups.data(), ps, clb);
    }
    else
    {
      // ps with shaderGroup 0 can be put in any shaderGroup, the vs will pull it in
      for (auto &shaderGroup : groups)
      {
        shaderGroup.removeGraphicsProgramWithMatchingPixelShader(groups.data(), ps, clb);
      }
    }

    // either if in shaderGroup 0 or in a shaderGroup other than 1 and combined with a vs that is not in the same shaderGroup
    // results being put into the global shaderGroup
    for (size_t i = 0; i < graphicsProgramTemplates.size(); ++i)
    {
      if (ps == graphicsProgramTemplates[i].pixelShader)
      {
        clb(GraphicsProgramID::make(0, i));
        graphicsProgramTemplates[i].vertexShader = ShaderID::Null();
        graphicsProgramTemplates[i].pixelShader = ShaderID::Null();
      }
    }
  }

  void clearGraphicsProgramsInstances()
  {
    graphicsProgramsHashMap.clear();
    graphicsProgramsHashMapDead = 0;
    graphicsProgramInstances.clear();
  }
  ProgramID incrementCachedGraphicsProgram(hash_key_t key)
  {
    uint32_t i = graphicsProgramsHashMap.findOr(key, ~uint32_t(0));
    if (i != ~uint32_t(0))
    {
      G_ASSERT(graphicsProgramInstances[i].getRefCount() > 0);
      graphicsProgramInstances[i].addRef();
      return ProgramID::asGraphicsProgram(0, i);
    }
    return ProgramID::Null();
  }
  hash_key_t getGraphicsProgramKey(InputLayoutID vdecl, ShaderID vs, ShaderID ps) const
  {
    constexpr int psBits = 24, vsBits = 24, vdeclBits = sizeof(hash_key_t) * 8 - psBits - vsBits; // 16 bits
    const hash_key_t vsId = int(vs.exportValue());
    const hash_key_t psId = int(ps.exportValue() + 1);
    G_ASSERTF(vsId < (1 << vsBits) && psId < (1 << psBits) && uint32_t(vdecl.get() + 1) < (1 << vdeclBits), "%d %d %d",
      uint32_t(vdecl.get()), vsId, psId);
    return hash_key_t(uint32_t(vdecl.get() + 1)) | (vsId << vdeclBits) | (psId << (vdeclBits + vsBits));
  }
  ProgramID instanciateGraphicsProgram(hash_key_t key, GraphicsProgramID gp, InputLayoutID il)
  {
    auto refEmpty = end(graphicsProgramInstances);
    auto refSame = eastl::find_if(begin(graphicsProgramInstances), end(graphicsProgramInstances), [&](auto &instance) {
      if (GraphicsProgramID::Null() == instance.internalProgram)
        refEmpty = &instance;
      return instance.internalProgram == gp && instance.inputLayout == il;
    });
    if (refSame != end(graphicsProgramInstances))
    {
      refSame->addRef();
      return ProgramID::asGraphicsProgram(0, refSame - begin(graphicsProgramInstances));
    }

    if (refEmpty == end(graphicsProgramInstances))
      refEmpty = graphicsProgramInstances.emplace(refEmpty);
    refEmpty->addRef();
    refEmpty->inputLayout = il;
    refEmpty->internalProgram = gp;
    const uint32_t pid = refEmpty - begin(graphicsProgramInstances);
    graphicsProgramsHashMap.emplace_if_missing(key, pid);
    return ProgramID::asGraphicsProgram(0, pid);
  }

  GraphicsProgramUsageInfo getUsageInfo(ProgramID program) const
  {
    GraphicsProgramUsageInfo info;
    const auto &inst = graphicsProgramInstances[program.getIndex()];
    info.input = inst.inputLayout;
    info.programId = inst.internalProgram;

    return info;
  }

  InputLayoutID getGraphicsProgramInstanceInputLayout(ProgramID prog) const
  {
    return graphicsProgramInstances[prog.getIndex()].inputLayout;
  }
  void removeInstanceFromHashmap(uint32_t id)
  {
    // we could store key inside GraphicsProgramInstance, but those 64 bit are only used during remove of last refCount, and it is very
    // rare
    graphicsProgramsHashMap.iterate([&](auto, uint32_t &v) {
      if (v == id)
      {
        v = ~uint32_t(0);
        graphicsProgramsHashMapDead++;
      }
    });
  }
  void removeAllDeads()
  {
    if (graphicsProgramsHashMapDead == 0)
      return;
    decltype(graphicsProgramsHashMap) temp;
    temp.reserve(eastl::max<int>(0, graphicsProgramsHashMap.size() - graphicsProgramsHashMapDead));
    graphicsProgramsHashMap.iterate([&](auto key, auto v) {
      if (v != ~uint32_t(0))
        temp.emplace_if_missing(key, v);
    });
    graphicsProgramsHashMap.swap(temp);
  }
  void removeGraphicsProgramInstance(ProgramID prog)
  {
    if (graphicsProgramInstances[prog.getIndex()].delRef())
    {
      removeInstanceFromHashmap(prog.getIndex());
      if (graphicsProgramsHashMapDead > graphicsProgramsHashMap.size() / 20) // too many deads, rehash
        removeAllDeads();
    }
  }

  void removeGraphicsProgramInstancesUsingMatchingTemplate(GraphicsProgramID templ)
  {
    for (auto &inst : graphicsProgramInstances)
    {
      if (inst.internalProgram == templ)
      {
        inst.deleteAllReferences();
        removeInstanceFromHashmap(&inst - begin(graphicsProgramInstances));
      }
    }
  }

  void setGroup(uint32_t index, ScriptedShadersBinDumpOwner *dump, ShaderID null_pixel_shader)
  {
    groups[index - 1].setup(groups.data(), dump, null_pixel_shader);
  }
  void dropGroup(uint32_t index) { groups[index - 1].clear(); }
  dxil::ShaderStage shaderFromIndex(uint32_t group_index, uint32_t shader_index, ShaderCodeType type, void *ident)
  {
    return groups[group_index - 1].shaderFromIndex(groups.data(), shader_index, type, ident);
  }
};

StageShaderModule shader_layout_to_module(const bindump::Mapper<dxil::Shader> *layout);
template <typename ModuleType>
inline ModuleType decode_shader_layout(const void *data)
{
  ModuleType result;
  auto *container = bindump::map<dxil::ShaderContainer>((const uint8_t *)data);
  if (!container)
    return result;

  if (container->type == dxil::StoredShaderType::combinedVertexShader)
  {
    if constexpr (eastl::is_same_v<ModuleType, VertexShaderModule>)
    {
      auto *combined = bindump::map<dxil::VertexShaderPipeline>(container->data.data());
      result = shader_layout_to_module(&*combined->vertexShader);
      result.ident.shaderHash = container->dataHash;
      result.ident.shaderSize = container->data.size();
      if (combined->geometryShader)
      {
        result.geometryShader = eastl::make_unique<StageShaderModule>(shader_layout_to_module(&*combined->geometryShader));
      }
      if (combined->hullShader)
      {
        result.hullShader = eastl::make_unique<StageShaderModule>(shader_layout_to_module(&*combined->hullShader));
      }
      if (combined->domainShader)
      {
        result.domainShader = eastl::make_unique<StageShaderModule>(shader_layout_to_module(&*combined->domainShader));
      }
    }
  }
  else if (container->type == dxil::StoredShaderType::meshShader)
  {
    if constexpr (eastl::is_same_v<ModuleType, VertexShaderModule>)
    {
      auto *combined = bindump::map<dxil::MeshShaderPipeline>(container->data.data());
      result = shader_layout_to_module(&*combined->meshShader);
      result.ident.shaderHash = container->dataHash;
      result.ident.shaderSize = container->data.size();
      if (combined->amplificationShader)
      {
        result.geometryShader = eastl::make_unique<StageShaderModule>(shader_layout_to_module(&*combined->amplificationShader));
      }
    }
  }
  else
  {
    auto *shader = bindump::map<dxil::Shader>(container->data.data());
    result = shader_layout_to_module(shader);
    result.ident.shaderHash = container->dataHash;
    result.ident.shaderSize = container->data.size();
  }
  return result;
}
StageShaderModule decode_shader_binary(const void *data, uint32_t size);
eastl::unique_ptr<VertexShaderModule> decode_vertex_shader(const void *data, uint32_t size);
eastl::unique_ptr<PixelShaderModule> decode_pixel_shader(const void *data, uint32_t size);

struct StageShaderModuleInBinaryRef : StageShaderModuleHeader
{
  eastl::span<const uint8_t> byteCode;
};
inline uint32_t offset_to_base(const void *base, const StageShaderModuleInBinaryRef &shader)
{
  return shader.byteCode.data() - reinterpret_cast<const uint8_t *>(base);
}
using PixelShaderModuleInBinaryRef = StageShaderModuleInBinaryRef;

struct VertexShaderModuleInBinaryRef : StageShaderModuleInBinaryRef
{
  // TODO remove shader identifier as its only relevant for the vertex shader it self
  StageShaderModuleInBinaryRef geometryShader;
  StageShaderModuleInBinaryRef hullShader;
  StageShaderModuleInBinaryRef domainShader;
};

StageShaderModuleInBinaryRef shader_layout_to_module_ref(const bindump::Mapper<dxil::Shader> *layout);
template <typename ModuleType>
inline ModuleType decode_shader_layout_ref(const void *data)
{
  ModuleType result;
  auto *container = bindump::map<dxil::ShaderContainer>((const uint8_t *)data);
  if (!container)
    return result;

  if (container->type == dxil::StoredShaderType::combinedVertexShader)
  {
    if constexpr (eastl::is_same_v<ModuleType, VertexShaderModuleInBinaryRef>)
    {
      auto *combined = bindump::map<dxil::VertexShaderPipeline>(container->data.data());
      static_cast<StageShaderModuleInBinaryRef &>(result) = shader_layout_to_module_ref(&*combined->vertexShader);
      result.ident.shaderHash = container->dataHash;
      result.ident.shaderSize = container->data.size();
      if (combined->geometryShader)
      {
        result.geometryShader = shader_layout_to_module_ref(&*combined->geometryShader);
      }
      if (combined->hullShader)
      {
        result.hullShader = shader_layout_to_module_ref(&*combined->hullShader);
      }
      if (combined->domainShader)
      {
        result.domainShader = shader_layout_to_module_ref(&*combined->domainShader);
      }
    }
  }
  else if (container->type == dxil::StoredShaderType::meshShader)
  {
    if constexpr (eastl::is_same_v<ModuleType, VertexShaderModuleInBinaryRef>)
    {
      auto *combined = bindump::map<dxil::MeshShaderPipeline>(container->data.data());
      static_cast<StageShaderModuleInBinaryRef &>(result) = shader_layout_to_module_ref(&*combined->meshShader);
      result.ident.shaderHash = container->dataHash;
      result.ident.shaderSize = container->data.size();
      if (combined->amplificationShader)
      {
        result.geometryShader = shader_layout_to_module_ref(&*combined->amplificationShader);
      }
    }
  }
  else
  {
    auto *shader = bindump::map<dxil::Shader>(container->data.data());
    static_cast<StageShaderModuleInBinaryRef &>(result) = shader_layout_to_module_ref(shader);
    result.ident.shaderHash = container->dataHash;
    result.ident.shaderSize = container->data.size();
  }
  return result;
}

StageShaderModuleInBinaryRef decode_shader_binary_ref(const void *data, uint32_t size);
VertexShaderModuleInBinaryRef decode_vertex_shader_ref(const void *data, uint32_t size);
PixelShaderModuleInBinaryRef decode_pixel_shader_ref(const void *data, uint32_t size);

class PipelineCache;
class ShaderProgramDatabase
{
  mutable OSReadWriteLock dataGuard;
#if D3D_HAS_RAY_TRACING
  eastl::vector<RaytraceProgram> raytracePrograms;
#endif
  eastl::vector<InputLayout> publicImputLayoutTable;

  ProgramID debugProgram = ProgramID::Null();
  ShaderID nullPixelShader = ShaderID::Null();
  bool disablePreCache = false;

  ShaderProgramGroups shaderProgramGroups;

  void initDebugProgram(DeviceContext &ctx);
  void initNullPixelShader(DeviceContext &ctx);
  ShaderID newRawVertexShader(DeviceContext &ctx, const dxil::ShaderHeader &header, dag::ConstSpan<uint8_t> byte_code);
  ShaderID newRawPixelShader(DeviceContext &ctx, const dxil::ShaderHeader &header, dag::ConstSpan<uint8_t> byte_code);

public:
  ProgramID newComputeProgram(DeviceContext &ctx, const void *data);
  ProgramID newGraphicsProgram(DeviceContext &ctx, InputLayoutID vdecl, ShaderID vs, ShaderID ps);
  InputLayoutID getInputLayoutForGraphicsProgram(ProgramID program);
  GraphicsProgramUsageInfo getGraphicsProgramForStateUpdate(ProgramID program);
  InputLayoutID registerInputLayoutInternal(DeviceContext &ctx, const InputLayout &layout);
  InputLayoutID registerInputLayout(DeviceContext &ctx, const InputLayout &layout);
  void setup(DeviceContext &ctx, bool disable_precache);
  void shutdown(DeviceContext &ctx);
  ShaderID newVertexShader(DeviceContext &ctx, const void *data);
  ShaderID newPixelShader(DeviceContext &ctx, const void *data);
  ProgramID getDebugProgram();
  void removeProgram(DeviceContext &ctx, ProgramID prog);
  void deleteVertexShader(DeviceContext &ctx, ShaderID shader);
  void deletePixelShader(DeviceContext &ctx, ShaderID shader);
  void updateVertexShaderName(DeviceContext &ctx, ShaderID shader, const char *name);
  void updatePixelShaderName(DeviceContext &ctx, ShaderID shader, const char *name);

#if D3D_HAS_RAY_TRACING
  ProgramID newRaytraceProgram(DeviceContext &ctx, const ShaderID *shader_ids, uint32_t shader_count,
    const RaytraceShaderGroup *shader_groups, uint32_t group_count, uint32_t max_recursion_depth);
#endif
  void registerShaderBinDump(DeviceContext &ctx, ScriptedShadersBinDumpOwner *dump);
  void getBindumpShader(DeviceContext &ctx, uint32_t index, ShaderCodeType type, void *ident);
};

namespace backend
{
class InputLayoutManager
{
  struct MaskToInternalMapEntry
  {
    uint32_t mask;
    InternalInputLayoutID id;
  };
  struct ExternalToInternalMapEntry
  {
    InputLayout layout;
    eastl::vector<MaskToInternalMapEntry> mapTable;
  };
  eastl::vector<InputLayout> internalLayoutTable;
  eastl::vector<ExternalToInternalMapEntry> externalToInternalMap;

public:
  void registerInputLayout(InputLayoutID id, const InputLayout &layout)
  {
    if (externalToInternalMap.size() <= id.get())
    {
      externalToInternalMap.resize(id.get() + 1);
    }
    auto &e = externalToInternalMap[id.get()];
    e.layout = layout;
  }

  InternalInputLayoutID remapInputLayout(InputLayoutID id, uint32_t layout_mask)
  {
    auto &e = externalToInternalMap[id.get()];
    auto ref = eastl::find_if(begin(e.mapTable), end(e.mapTable), [layout_mask](auto &value) { return value.mask == layout_mask; });
    if (end(e.mapTable) != ref)
    {
      return ref->id;
    }
    auto internalID = addInternalLayout(e.layout.getLayoutForAttributeUse(layout_mask));
    MaskToInternalMapEntry v{layout_mask, internalID};
    e.mapTable.push_back(v);
    return internalID;
  }

  const InputLayout &getInputLayout(InternalInputLayoutID id) const { return internalLayoutTable[id.get()]; }

  InternalInputLayoutID addInternalLayout(const InputLayout &layout)
  {
    auto ref = eastl::find(begin(internalLayoutTable), end(internalLayoutTable), layout);
    if (ref == end(internalLayoutTable))
    {
      ref = internalLayoutTable.insert(end(internalLayoutTable), layout);
    }
    return InternalInputLayoutID{int(ref - begin(internalLayoutTable))};
  }
};

class ScriptedShadersBinDumpManager
{
  struct DecompressedGroup
  {
    Tab<uint8_t> uncompressed;
    bindump::Mapper<shader_layout::ShGroup> *shaderGroup = nullptr;
  };
  struct ScriptedShadersBinDumpState
  {
    ScriptedShadersBinDumpOwner *owner = nullptr;
    eastl::unique_ptr<DecompressedGroup[]> decompressedGroups;
  };
  ScriptedShadersBinDumpState dumps[max_scripted_shaders_bin_groups]{};

public:
  void setDumpOfGroup(uint32_t shaderGroup, ScriptedShadersBinDumpOwner *dump)
  {
    dumps[shaderGroup].owner = dump;
    dumps[shaderGroup].decompressedGroups = eastl::make_unique<DecompressedGroup[]>(dump->getDump()->shGroups.size());
  }
  void resetDumpOfGroup(uint32_t shaderGroup)
  {
    dumps[shaderGroup].owner = nullptr;
    dumps[shaderGroup].decompressedGroups.reset();
  }
  // returned memory range is valid until the end of the frame, after that its undefined.
  eastl::span<uint8_t> getShaderByteCode(uint32_t shaderGroup, uint32_t compression_group, uint32_t compression_index)
  {
    auto &dump = dumps[shaderGroup];
    if (!dump.decompressedGroups)
    {
      return {nullptr, nullptr};
    }
    auto &compGroup = dump.decompressedGroups[compression_group];
    if (!compGroup.shaderGroup)
    {
      debug("DX12: Decompressing shader shaderGroup %u of dump %p...", compression_group, dump.owner);
      compGroup.shaderGroup = dump.owner->getDump()->shGroups[compression_group].decompress(compGroup.uncompressed,
        bindump::BehaviorOnUncompressed::Reference, dump.owner->getDecompressionDict());
      ByteUnits uncompressedByteSize = compGroup.uncompressed.size();
      debug("DX12: ...decompressed into %.2f %s...", uncompressedByteSize.units(), uncompressedByteSize.name());
    }
    auto &shader = compGroup.shaderGroup->shaders[compression_index];
    return {shader.begin(), shader.end()};
  }
  // currently always evicts cache
  void evictDecompressionCache()
  {
    for (auto &dump : dumps)
    {
      if (!dump.decompressedGroups)
        continue;
      for (uint32_t i = 0; i < dump.owner->getDump()->shGroups.size(); ++i)
      {
        dump.decompressedGroups[i].uncompressed.clear();
        dump.decompressedGroups[i].shaderGroup = nullptr;
      }
    }
  }
};

struct StageShaderModuleBytecode
{
  eastl::unique_ptr<uint8_t[]> bytecode;
  size_t bytecodeSize = 0;
};

struct VertexShaderModuleBytecode : StageShaderModuleBytecode
{
  eastl::unique_ptr<StageShaderModuleBytecode[]> subShaders;
};

using PixelShaderModuleBytecode = StageShaderModuleBytecode;

struct VertexShaderModuleBytecodeRef
{
  const VertexShaderModuleBytecode *bytecode = nullptr;
};

struct PixelShaderModuleBytecodeRef
{
  const PixelShaderModuleBytecode *bytecode = nullptr;
};

struct StageShaderModuleBytcodeInDumpOffsets
{
  uint32_t shaderOffset = 0;
  uint32_t shaderSize = 0;
};

struct StageShaderModuleBytcodeInDump : StageShaderModuleBytcodeInDumpOffsets
{
  uint16_t compressionGroup = 0;
  uint16_t compressionIndex = 0;
};

struct VertexShaderModuleBytcodeInDump : StageShaderModuleBytcodeInDump
{
  eastl::unique_ptr<StageShaderModuleBytcodeInDumpOffsets[]> subShaders;
};

using PixelShaderModuleBytcodeInDump = StageShaderModuleBytcodeInDump;

struct VertexShaderModuleBytcodeInDumpRef
{
  uint32_t shaderGroup = 0;
  const VertexShaderModuleBytcodeInDump *bytecode = nullptr;
};

struct PixelShaderModuleBytcodeInDumpRef
{
  uint32_t shaderGroup = 0;
  const PixelShaderModuleBytcodeInDump *bytecode = nullptr;
};

struct VertexShaderModuleHeader
{
  dxil::HashValue hash = {};
  dxil::ShaderHeader header = {};
  eastl::unique_ptr<dxil::ShaderHeader[]> subShaders;
  eastl::string debugName;
  bool hasGsOrAs : 1;
  bool hasDsAndHs : 1;

  VertexShaderModuleHeader() : hasGsOrAs{false}, hasDsAndHs{false} {}
};

struct PixelShaderModuleHeader
{
  dxil::HashValue hash = {};
  dxil::ShaderHeader header = {};
  eastl::string debugName;
};

struct VertexShaderModuleRefStore
{
  VertexShaderModuleHeader &header;
  eastl::variant<VertexShaderModuleBytecodeRef, VertexShaderModuleBytcodeInDumpRef> bytecode;
};

inline const dxil::ShaderHeader *get_gs(const VertexShaderModuleRefStore &ref)
{
  return ref.header.hasGsOrAs ? &ref.header.subShaders[0] : nullptr;
}

inline const dxil::ShaderHeader *get_hs(const VertexShaderModuleRefStore &ref)
{
  return ref.header.hasDsAndHs ? &ref.header.subShaders[ref.header.hasGsOrAs ? 1 : 0] : nullptr;
}

inline const dxil::ShaderHeader *get_ds(const VertexShaderModuleRefStore &ref)
{
  return ref.header.hasDsAndHs ? &ref.header.subShaders[ref.header.hasGsOrAs ? 2 : 1] : nullptr;
}

inline const dxil::ShaderHeader *get_as(const VertexShaderModuleRefStore &ref)
{
  return ref.header.hasGsOrAs ? &ref.header.subShaders[0] : nullptr;
}

#if _TARGET_XBOXONE
// XB1 has no mesh shader stage
inline constexpr bool is_mesh(const VertexShaderModuleRefStore &) { return false; }
#else
inline bool is_mesh(const VertexShaderModuleRefStore &ref)
{
  return dxil::ShaderStage::MESH == static_cast<dxil::ShaderStage>(ref.header.header.shaderType);
}
#endif

struct PixelShaderModuleRefStore
{
  const PixelShaderModuleHeader &header;
  eastl::variant<PixelShaderModuleBytecodeRef, PixelShaderModuleBytcodeInDumpRef> bytecode;
};

class ShaderModuleManager : public ScriptedShadersBinDumpManager
{
  struct GroupZeroVertexShaderModule
  {
    VertexShaderModuleHeader header;
    VertexShaderModuleBytecode bytecode;
  };
  struct GroupVertexShaderModule
  {
    VertexShaderModuleHeader header;
    VertexShaderModuleBytcodeInDump bytecode;
  };
  struct GroupZeroPixelShaderModule
  {
    PixelShaderModuleHeader header;
    PixelShaderModuleBytecode bytecode;
  };
  struct GroupPixelShaderModule
  {
    PixelShaderModuleHeader header;
    PixelShaderModuleBytcodeInDump bytecode;
  };
  struct
  {
    eastl::vector<eastl::unique_ptr<GroupZeroVertexShaderModule>> vertex;
    eastl::vector<eastl::unique_ptr<GroupZeroPixelShaderModule>> pixel;
  } shaderGroupZero;
  struct
  {
    eastl::vector<eastl::unique_ptr<GroupVertexShaderModule>> vertex;
    eastl::vector<eastl::unique_ptr<GroupPixelShaderModule>> pixel;
  } shaderGroup[max_scripted_shaders_bin_groups - 1];

  eastl::span<uint8_t> getBytecode(const PixelShaderModuleBytecodeRef &ref)
  {
    return {ref.bytecode->bytecode.get(), ref.bytecode->bytecode.get() + ref.bytecode->bytecodeSize};
  }

  eastl::span<uint8_t> getBytecode(const PixelShaderModuleBytcodeInDumpRef &ref)
  {
    auto binary = getShaderByteCode(ref.shaderGroup, ref.bytecode->compressionGroup, ref.bytecode->compressionIndex);
    return binary.subspan(ref.bytecode->shaderOffset, ref.bytecode->shaderSize);
  }

  eastl::span<uint8_t> getVsBytecode(const VertexShaderModuleHeader &, const VertexShaderModuleBytecodeRef &ref)
  {
    return {ref.bytecode->bytecode.get(), ref.bytecode->bytecode.get() + ref.bytecode->bytecodeSize};
  }
  eastl::span<uint8_t> getVsBytecode(const VertexShaderModuleHeader &, const VertexShaderModuleBytcodeInDumpRef &ref)
  {
    auto binary = getShaderByteCode(ref.shaderGroup, ref.bytecode->compressionGroup, ref.bytecode->compressionIndex);
    return binary.subspan(ref.bytecode->shaderOffset, ref.bytecode->shaderSize);
  }
  eastl::span<uint8_t> getMsBytecode(const VertexShaderModuleHeader &header, const VertexShaderModuleBytecodeRef &ref)
  {
    return getVsBytecode(header, ref);
  }
  eastl::span<uint8_t> getMsBytecode(const VertexShaderModuleHeader &header, const VertexShaderModuleBytcodeInDumpRef &ref)
  {
    return getVsBytecode(header, ref);
  }
  eastl::span<uint8_t> getGsBytecode(const VertexShaderModuleHeader &, const VertexShaderModuleBytecodeRef &ref)
  {
    return getSubShaderBytecode(0, ref);
  }
  eastl::span<uint8_t> getGsBytecode(const VertexShaderModuleHeader &, const VertexShaderModuleBytcodeInDumpRef &ref)
  {
    return getSubShaderBytecode(0, ref);
  }
  eastl::span<uint8_t> getAsBytecode(const VertexShaderModuleHeader &header, const VertexShaderModuleBytecodeRef &ref)
  {
    return getGsBytecode(header, ref);
  }
  eastl::span<uint8_t> getAsBytecode(const VertexShaderModuleHeader &header, const VertexShaderModuleBytcodeInDumpRef &ref)
  {
    return getGsBytecode(header, ref);
  }
  eastl::span<uint8_t> getHsBytecode(const VertexShaderModuleHeader &header, const VertexShaderModuleBytecodeRef &ref)
  {
    return getSubShaderBytecode(header.hasGsOrAs ? 1 : 0, ref);
  }
  eastl::span<uint8_t> getHsBytecode(const VertexShaderModuleHeader &header, const VertexShaderModuleBytcodeInDumpRef &ref)
  {
    return getSubShaderBytecode(header.hasGsOrAs ? 1 : 0, ref);
  }
  eastl::span<uint8_t> getDsBytecode(const VertexShaderModuleHeader &header, const VertexShaderModuleBytecodeRef &ref)
  {
    return getSubShaderBytecode(header.hasGsOrAs ? 2 : 1, ref);
  }
  eastl::span<uint8_t> getDsBytecode(const VertexShaderModuleHeader &header, const VertexShaderModuleBytcodeInDumpRef &ref)
  {
    return getSubShaderBytecode(header.hasGsOrAs ? 2 : 1, ref);
  }
  eastl::span<uint8_t> getSubShaderBytecode(uint32_t sub_shader_index, const VertexShaderModuleBytecodeRef &ref)
  {
    auto &subShader = ref.bytecode->subShaders[sub_shader_index];
    return {subShader.bytecode.get(), subShader.bytecode.get() + subShader.bytecodeSize};
  }
  eastl::span<uint8_t> getSubShaderBytecode(uint32_t sub_shader_index, const VertexShaderModuleBytcodeInDumpRef &ref)
  {
    auto &subShader = ref.bytecode->subShaders[sub_shader_index];
    auto binary = getShaderByteCode(ref.shaderGroup, ref.bytecode->compressionGroup, ref.bytecode->compressionIndex);
    return binary.subspan(subShader.shaderOffset, subShader.shaderSize);
  }

public:
  void addVertexShader(ShaderID id, VertexShaderModule *module);
  void addPixelShader(ShaderID id, PixelShaderModule *module);
  const dxil::HashValue &getVertexShaderHash(ShaderID id) const;
  void setVertexShaderName(ShaderID id, eastl::span<const char> name);
  const dxil::HashValue &getPixelShaderHash(ShaderID id) const;
  void setPixelShaderName(ShaderID id, eastl::span<const char> name);
  VertexShaderModuleRefStore getVertexShader(ShaderID id);
  PixelShaderModuleRefStore getPixelShader(ShaderID id);

  eastl::span<uint8_t> getBytecode(const PixelShaderModuleRefStore &ref)
  {
    return eastl::visit([this](auto &r) -> eastl::span<uint8_t> { return this->getBytecode(r); }, ref.bytecode);
  }

  eastl::span<uint8_t> getVsBytecode(const VertexShaderModuleRefStore &ref)
  {
    return eastl::visit([this, &ref](auto &r) -> eastl::span<uint8_t> { return this->getVsBytecode(ref.header, r); }, ref.bytecode);
  }

  eastl::span<uint8_t> getMsBytecode(const VertexShaderModuleRefStore &ref)
  {
    return eastl::visit([this, &ref](auto &r) -> eastl::span<uint8_t> { return this->getMsBytecode(ref.header, r); }, ref.bytecode);
  }

  eastl::span<uint8_t> getGsBytecode(const VertexShaderModuleRefStore &ref)
  {
    return eastl::visit([this, &ref](auto &r) -> eastl::span<uint8_t> { return this->getGsBytecode(ref.header, r); }, ref.bytecode);
  }

  eastl::span<uint8_t> getAsBytecode(const VertexShaderModuleRefStore &ref)
  {
    return eastl::visit([this, &ref](auto &r) -> eastl::span<uint8_t> { return this->getAsBytecode(ref.header, r); }, ref.bytecode);
  }

  eastl::span<uint8_t> getHsBytecode(const VertexShaderModuleRefStore &ref)
  {
    return eastl::visit([this, &ref](auto &r) -> eastl::span<uint8_t> { return this->getHsBytecode(ref.header, r); }, ref.bytecode);
  }

  eastl::span<uint8_t> getDsBytecode(const VertexShaderModuleRefStore &ref)
  {
    return eastl::visit([this, &ref](auto &r) -> eastl::span<uint8_t> { return this->getDsBytecode(ref.header, r); }, ref.bytecode);
  }

  using AnyShaderModuleUniquePointer =
    eastl::variant<eastl::unique_ptr<GroupZeroVertexShaderModule>, eastl::unique_ptr<GroupVertexShaderModule>,
      eastl::unique_ptr<GroupZeroPixelShaderModule>, eastl::unique_ptr<GroupPixelShaderModule>>;

  AnyShaderModuleUniquePointer releaseVertexShader(ShaderID id);
  AnyShaderModuleUniquePointer releasePixelShader(ShaderID id);
  void resetDumpOfGroup(uint32_t group_index);
  void reserveVertexShaderRange(uint32_t group_index, uint32_t count);
  void setVertexShaderCompressionGroup(uint32_t group_index, uint32_t index, const dxil::HashValue &hash, uint32_t compression_group,
    uint32_t compression_index);
  void setPixelShaderCompressionGroup(uint32_t group_index, uint32_t index, const dxil::HashValue &hash, uint32_t compression_group,
    uint32_t compression_index);
};
}; // namespace backend
} // namespace drv3d_dx12