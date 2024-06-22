#include "Metal/MTLPixelFormat.hpp"
#include "Metal/MTLResource.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d11_device.hpp"
#include "dxgi_interfaces.h"
#include "dxmt_buffer_pool.hpp"
#include "mtld11_resource.hpp"
#include "Metal/MTLBuffer.hpp"
#include "Metal/MTLTexture.hpp"
#include "objc_pointer.hpp"
#include <vector>

namespace dxmt {

#pragma region DynamicViewBindable
struct IMTLNotifiedBindable : public IMTLBindable {
  virtual void NotifyObserver(MTL::Buffer *resource) = 0;
};

template <typename BindableMap, typename GetArgumentData_,
          typename ReleaseCallback>
class DynamicBinding : public ComObject<IMTLNotifiedBindable> {
private:
  Com<IUnknown> parent;
  BufferSwapCallback observer;
  BindableMap fn;
  GetArgumentData_ get_argument_data;
  ReleaseCallback cb;

public:
  DynamicBinding(IUnknown *parent, BufferSwapCallback &&observer,
                 BindableMap &&fn, GetArgumentData_ &&get_argument_data,
                 ReleaseCallback &&cb)
      : ComObject(), parent(parent), observer(std::move(observer)),
        fn(std::forward<BindableMap>(fn)),
        get_argument_data(std::forward<GetArgumentData_>(get_argument_data)),
        cb(std::forward<ReleaseCallback>(cb)) {}

  ~DynamicBinding() { std::invoke(cb, this); }

  HRESULT QueryInterface(REFIID riid, void **ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMTLBindable)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    return E_NOINTERFACE;
  }

  BindingRef GetBinding(uint64_t x) override { return std::invoke(fn, x); };

  ArgumentData GetArgumentData() override {
    return std::invoke(get_argument_data);
  }

  bool GetContentionState(uint64_t finishedSeqId) override { return true; }

  void GetLogicalResourceOrView(REFIID riid,
                                void **ppLogicalResource) override {
    parent->QueryInterface(riid, ppLogicalResource);
  };

  void NotifyObserver(MTL::Buffer *resource) override { observer(resource); };
};
#pragma endregion

#pragma region DynamicBuffer

class DynamicBuffer
    : public TResourceBase<tag_buffer, IMTLDynamicBuffer, IMTLDynamicBindable> {
private:
  Obj<MTL::Buffer> buffer_dynamic;
  uint64_t buffer_handle;
  void *buffer_mapped;

  std::vector<IMTLNotifiedBindable *> observers;

  using SRVBase = TResourceViewBase<tag_shader_resource_view<DynamicBuffer>,
                                    IMTLDynamicBindable>;

  class SRV : public SRVBase {
  private:
  public:
    SRV(const tag_shader_resource_view<>::DESC_S *pDesc,
        DynamicBuffer *pResource, IMTLD3D11Device *pDevice)
        : SRVBase(pDesc, pResource, pDevice) {}

    ~SRV() {
      auto &vec = resource->weak_srvs;
      vec.erase(std::remove(vec.begin(), vec.end(), this), vec.end());
    }

    void GetBindable(IMTLBindable **ppResource,
                     BufferSwapCallback &&onBufferSwap) override {
      IMTLNotifiedBindable *ret = ref(new DynamicBinding(
          static_cast<ID3D11ShaderResourceView *>(this),
          std::move(onBufferSwap),
          [](uint64_t) {
            assert(0 && "todo: prepare dynamic buffer srv view");
            return BindingRef(std::nullopt);
          },
          []() {
            assert(0 && "todo: prepare dynamic buffer srv view");
            return ArgumentData(0uL, 0);
          },
          [u = this->resource.ptr()](IMTLNotifiedBindable *_this) {
            u->RemoveObserver(_this);
          }));
      resource->AddObserver(ret);
      *ppResource = ret;
    };

    void RotateView() {

    };
  };

  std::vector<SRV *> weak_srvs;

  std::unique_ptr<BufferPool> pool;

public:
  DynamicBuffer(const tag_buffer::DESC_S *desc,
                const D3D11_SUBRESOURCE_DATA *pInitialData,
                IMTLD3D11Device *device)
      : TResourceBase<tag_buffer, IMTLDynamicBuffer, IMTLDynamicBindable>(
            desc, device) {
    auto metal = device->GetMTLDevice();
    auto options = MTL::ResourceCPUCacheModeWriteCombined |
                   MTL::ResourceHazardTrackingModeUntracked;
    buffer_dynamic = transfer(metal->newBuffer(desc->ByteWidth, options));
    if (pInitialData) {
      memcpy(buffer_dynamic->contents(), pInitialData->pSysMem,
             desc->ByteWidth);
    }
    buffer_handle = buffer_dynamic->gpuAddress();
    buffer_mapped = buffer_dynamic->contents();
    pool = std::make_unique<BufferPool>(metal, desc->ByteWidth, options);
  }

  void *GetMappedMemory(UINT *pBytesPerRow) override { return buffer_mapped; };

  void GetBindable(IMTLBindable **ppResource,
                   BufferSwapCallback &&onBufferSwap) override {
    auto ret = ref(new DynamicBinding(
        static_cast<ID3D11Buffer *>(this), std::move(onBufferSwap),
        [this](uint64_t) { return BindingRef(buffer_dynamic.ptr()); },
        [this]() { return ArgumentData(this->buffer_handle); },
        [this](IMTLNotifiedBindable *_binding) {
          this->RemoveObserver(_binding);
        }));
    AddObserver(ret);
    *ppResource = ret;
  };

  void RotateBuffer(IMTLDynamicBufferExchange *exch) override {
    exch->ExchangeFromPool(&buffer_dynamic, &buffer_handle, &buffer_mapped,
                           pool.get());
    for (auto srv : weak_srvs) {
      srv->RotateView();
    }
    for (auto &observer : observers) {
      observer->NotifyObserver(buffer_dynamic.ptr());
    }
  };

  void AddObserver(IMTLNotifiedBindable *pBindable) {
    // assert(observers.insert(pBindable).second && "otherwise already added");
    observers.push_back(pBindable);
  }

  void RemoveObserver(IMTLNotifiedBindable *pBindable) {
    // assert(observers.erase(pBindable) == 1 &&
    //        "it must be 1 unless the destructor called twice");
    auto &vec = observers;
    vec.erase(std::remove(vec.begin(), vec.end(), pBindable), vec.end());
  }

  HRESULT CreateShaderResourceView(const D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc,
                                   ID3D11ShaderResourceView **ppView) override {
    D3D11_SHADER_RESOURCE_VIEW_DESC finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc,
                                                    &finalDesc))) {
      return E_INVALIDARG;
    }
    if (finalDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFER &&
        finalDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFEREX) {
      return E_FAIL;
    }
    if (!ppView) {
      return S_FALSE;
    }
    ERR("DynamicBuffer srv not impl");
    return E_NOTIMPL;
    // TODO: polymorphism: buffer/byteaddress/structured
    auto srv = ref(new SRV(&finalDesc, this, m_parent));
    // assert(weak_srvs.insert(srv).second);
    weak_srvs.push_back(srv);
    *ppView = srv;
    srv->RotateView();
    return S_OK;
  };
};

#pragma endregion

HRESULT
CreateDynamicBuffer(IMTLD3D11Device *pDevice, const D3D11_BUFFER_DESC *pDesc,
                    const D3D11_SUBRESOURCE_DATA *pInitialData,
                    ID3D11Buffer **ppBuffer) {
  *ppBuffer = ref(new DynamicBuffer(pDesc, pInitialData, pDevice));
  return S_OK;
}

#pragma region DynamicTexture

class DynamicTexture2D
    : public TResourceBase<tag_texture_2d, IMTLDynamicBuffer> {
private:
  Obj<MTL::Buffer> buffer;
  uint64_t buffer_handle;
  void *buffer_mapped;
  size_t bytes_per_row;

  using SRVBase = TResourceViewBase<tag_shader_resource_view<DynamicTexture2D>,
                                    IMTLDynamicBindable>;

  class SRV : public SRVBase {
    Obj<MTL::Texture> view;
    MTL::ResourceID view_handle;
    Obj<MTL::TextureDescriptor> view_desc;

  public:
    SRV(const tag_shader_resource_view<>::DESC_S *pDesc,
        DynamicTexture2D *pResource, IMTLD3D11Device *pDevice,
        Obj<MTL::TextureDescriptor> &&view_desc)
        : SRVBase(pDesc, pResource, pDevice), view_desc(std::move(view_desc)) {}

    ~SRV() {
      auto vec = resource->weak_srvs;
      vec.erase(std::remove(vec.begin(), vec.end(), this), vec.end());
    }

    void GetBindable(IMTLBindable **ppResource,
                     BufferSwapCallback &&onBufferSwap) override {
      auto ret = ref(new DynamicBinding(
          static_cast<ID3D11ShaderResourceView *>(this),
          std::move(onBufferSwap),
          [this](uint64_t) { return BindingRef(this->view.ptr()); },
          [this]() {
            return ArgumentData(this->view_handle, this->view.ptr());
          },
          [u = this->resource.ptr()](IMTLNotifiedBindable *_this) {
            u->RemoveObserver(_this);
          }));
      resource->AddObserver(ret);
      *ppResource = ret;
    }

    struct ViewCache {
      Obj<MTL::Texture> view;
      MTL::ResourceID view_handle;
    };

    // FIXME: use LRU cache instead!
    std::unordered_map<uint64_t, ViewCache> cache;

    void RotateView() {
      auto insert = cache.insert({resource->buffer_handle, {}});
      auto &item = insert.first->second;
      if (insert.second) {
        item.view = transfer(resource->buffer->newTexture(
            view_desc, 0, resource->bytes_per_row));
        item.view_handle = item.view->gpuResourceID();
      }
      view = item.view;
      view_handle = item.view_handle;
    };
  };

  std::vector<IMTLNotifiedBindable *> observers;
  std::vector<SRV *> weak_srvs;
  std::unique_ptr<BufferPool> pool_;

public:
  DynamicTexture2D(const tag_texture_2d::DESC_S *desc,
                   Obj<MTL::Buffer> &&buffer, IMTLD3D11Device *device,
                   UINT bytesPerRow)
      : TResourceBase<tag_texture_2d, IMTLDynamicBuffer>(desc, device),
        buffer(std::move(buffer)), buffer_handle(this->buffer->gpuAddress()),
        buffer_mapped(this->buffer->contents()), bytes_per_row(bytesPerRow) {

    pool_ = std::make_unique<BufferPool>(device->GetMTLDevice(),
                                         this->buffer->length(),
                                         this->buffer->resourceOptions());
  }

  void *GetMappedMemory(UINT *pBytesPerRow) override {
    *pBytesPerRow = bytes_per_row;
    return buffer_mapped;
  };

  void RotateBuffer(IMTLDynamicBufferExchange *exch) override {
    exch->ExchangeFromPool(&buffer, &buffer_handle, &buffer_mapped,
                           pool_.get());
    for (auto srv : weak_srvs) {
      srv->RotateView();
    }
    for (auto &observer : observers) {
      observer->NotifyObserver(buffer.ptr());
    }
  };

  void AddObserver(IMTLNotifiedBindable *pBindable) {
    // assert(observers.insert(pBindable).second && "otherwise already added");
    observers.push_back(pBindable);
  }

  void RemoveObserver(IMTLNotifiedBindable *pBindable) {
    // assert(observers.erase(pBindable) == 1 &&
    //        "it must be 1 unless the destructor called twice");
    auto vec = observers;
    vec.erase(std::remove(vec.begin(), vec.end(), pBindable), vec.end());
  }

  HRESULT CreateShaderResourceView(const D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc,
                                   ID3D11ShaderResourceView **ppView) override {
    D3D11_SHADER_RESOURCE_VIEW_DESC finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc,
                                                    &finalDesc))) {
      return E_INVALIDARG;
    }
    if (finalDesc.ViewDimension != D3D10_SRV_DIMENSION_TEXTURE2D) {
      ERR("Only 2d texture SRV can be created on dynamic texture");
      return E_FAIL;
    }
    if (finalDesc.Texture2D.MostDetailedMip != 0 ||
        finalDesc.Texture2D.MipLevels != 1) {
      ERR("2d texture SRV must be non-mipmapped on dynamic texture");
      return E_FAIL;
    }
    if (!ppView) {
      return S_FALSE;
    }
    MTL_FORMAT_DESC format;
    Com<IMTLDXGIAdatper> adapter;
    m_parent->GetAdapter(&adapter);
    if (FAILED(adapter->QueryFormatDesc(finalDesc.Format, &format))) {
      return E_FAIL;
    }

    auto desc = transfer(MTL::TextureDescriptor::alloc()->init());
    desc->setTextureType(MTL::TextureType2D);
    desc->setWidth(this->desc.Width);
    desc->setHeight(this->desc.Height);
    desc->setDepth(1);
    desc->setArrayLength(1);
    desc->setMipmapLevelCount(1);
    desc->setSampleCount(1);
    desc->setUsage(MTL::TextureUsageShaderRead);
    desc->setStorageMode(MTL::StorageModeShared);
    desc->setCpuCacheMode(MTL::CPUCacheModeWriteCombined);
    desc->setPixelFormat(format.PixelFormat);
    auto srv = ref(new SRV(&finalDesc, this, m_parent, std::move(desc)));
    weak_srvs.push_back(srv);
    *ppView = srv;
    srv->RotateView();
    return S_OK;
  };
};

#pragma endregion

HRESULT
CreateDynamicTexture2D(IMTLD3D11Device *pDevice,
                       const D3D11_TEXTURE2D_DESC *pDesc,
                       const D3D11_SUBRESOURCE_DATA *pInitialData,
                       ID3D11Texture2D **ppTexture) {
  Com<IMTLDXGIAdatper> adapter;
  pDevice->GetAdapter(&adapter);
  MTL_FORMAT_DESC format;
  adapter->QueryFormatDesc(pDesc->Format, &format);
  if (format.IsCompressed) {
    return E_FAIL;
  }
  if (format.PixelFormat == MTL::PixelFormatInvalid) {
    return E_FAIL;
  }
  Obj<MTL::TextureDescriptor> textureDescriptor;
  D3D11_TEXTURE2D_DESC finalDesc;
  if (FAILED(CreateMTLTextureDescriptor(pDevice, pDesc, &finalDesc,
                                        &textureDescriptor))) {
    return E_INVALIDARG;
  }
  uint32_t bytesPerRow, bytesPerImage, bufferLen;
  if (FAILED(GetLinearTextureLayout(pDevice, &finalDesc, 0, &bytesPerRow,
                                    &bytesPerImage, &bufferLen))) {
    return E_FAIL;
  }
  auto metal = pDevice->GetMTLDevice();
  auto buffer = transfer(metal->newBuffer(
      bufferLen, MTL::ResourceOptionCPUCacheModeWriteCombined));
  if (pInitialData) {
    assert(pInitialData->SysMemPitch == bytesPerRow);
    memcpy(buffer->contents(), pInitialData->pSysMem, bufferLen);
  }
  *ppTexture =
      ref(new DynamicTexture2D(pDesc, std::move(buffer), pDevice, bytesPerRow));
  return S_OK;
}

} // namespace dxmt