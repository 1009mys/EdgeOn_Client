#include "videocell.h"
#include <QApplication>
#include <QPainter>
#include <QInputDialog>
#include <QMenu>
#include <QDrag>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QHash>
#include <QUrl>
#include <QDebug>
#include <QDateTime>
#include <QtGlobal>
#include <atomic>
#include <opencv2/cudaimgproc.hpp>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cuda_runtime_api.h>
#include <cuda_d3d11_interop.h>
#include <wrl/client.h>
#endif

static constexpr int STATUS_H = 14;
static constexpr const char* CELL_DRAG_MIME = "application/x-rtsp-viewer-cell-id";
static constexpr const char* CAMERA_ID_MIME = "application/x-edgeon-camera-id";

namespace {

bool isInteropDisabledByEnv() {
    bool ok = false;
    const int value = qEnvironmentVariableIntValue("EDGEON_ENABLE_CUDA_D3D_INTEROP", &ok);
    // 기본은 interop 활성화, 명시적으로 0일 때만 비활성화한다.
    return ok && value == 0;
}

std::atomic<bool> g_gpuInteropGloballyDisabled{isInteropDisabledByEnv()};
constexpr qint64 kInteropRetryBaseMs = 500;
constexpr qint64 kInteropRetryMaxMs = 5000;

QRectF aspectFitRect(const QSizeF& src, const QSizeF& dst) {
    if (src.isEmpty() || dst.isEmpty())
        return {};

    const qreal scale = qMin(dst.width() / src.width(), dst.height() / src.height());
    const QSizeF scaled(src.width() * scale, src.height() * scale);
    return {
        (dst.width() - scaled.width()) * 0.5,
        (dst.height() - scaled.height()) * 0.5,
        scaled.width(),
        scaled.height()
    };
}

QRect videoRectFor(const QSize& size) {
    const int h = qMax(1, size.height() - STATUS_H);
    return {0, 0, size.width(), h};
}

QRect statusRectFor(const QSize& size) {
    const int y = qMax(0, size.height() - STATUS_H);
    return {0, y, size.width(), STATUS_H};
}

QRect statusLabelRectFor(const QSize& size) {
    constexpr int borderInset = 4;
    constexpr int topInset = 2;
    constexpr int bottomInset = 4;
    return statusRectFor(size).adjusted(borderInset, topInset, -borderInset, -bottomInset);
}

bool isDirectRenderableFormat(const QImage::Format format) {
    static const QHash<QImage::Format, bool> directFormats{
        {QImage::Format_ARGB32,                 true},
        {QImage::Format_RGB32,                  true},
        {QImage::Format_ARGB32_Premultiplied,   true}
    };
    return directFormats.value(format, false);
}

#ifdef Q_OS_WIN

using Microsoft::WRL::ComPtr;

static D2D1_SIZE_U nativeClientPixelSize(const HWND hwnd) {
    RECT rc{};
    if (!hwnd || !GetClientRect(hwnd, &rc))
        return D2D1::SizeU(1, 1);

    const auto w = static_cast<UINT32>(qMax<LONG>(1, rc.right - rc.left));
    const auto h = static_cast<UINT32>(qMax<LONG>(1, rc.bottom - rc.top));
    return D2D1::SizeU(w, h);
}

static D2D1_COLOR_F toD2D(const QColor& color) {
    return D2D1::ColorF(color.redF(), color.greenF(), color.blueF(), color.alphaF());
}

void logInteropFailure(const int cellId, const char* stage) {
    qWarning() << "[VideoCell][Interop] cell" << (cellId + 1)
               << "failed at" << stage;
}

void logInteropFailure(const int cellId, const char* stage, const cudaError_t errorCode) {
    qWarning() << "[VideoCell][Interop] cell" << (cellId + 1)
               << "failed at" << stage
               << "cudaError=" << cudaGetErrorString(errorCode);
}

#endif

} // namespace

#ifdef Q_OS_WIN
struct VideoCell::RenderState {
    ComPtr<ID2D1Factory1> factory;
    ComPtr<IDWriteFactory> dwriteFactory;
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2dContext;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID2D1Bitmap1> targetBitmap;
    ComPtr<ID2D1Bitmap1> frameBitmap;
    ComPtr<ID3D11Texture2D> gpuFrameTexture;
    ComPtr<IDXGISurface> gpuFrameSurface;
    cudaGraphicsResource* cudaFrameResource{nullptr};
    int gpuFrameWidth{0};
    int gpuFrameHeight{0};
    bool gpuInteropDisabled{false};
    int interopFailureCount{0};
    qint64 interopRetryAtMs{0};
    ComPtr<ID2D1SolidColorBrush> activeBorderBrush;
    ComPtr<ID2D1SolidColorBrush> inactiveBorderBrush;
    ComPtr<ID2D1SolidColorBrush> dragBorderBrush;
    ComPtr<ID2D1SolidColorBrush> placeholderBrush;
    ComPtr<ID2D1SolidColorBrush> channelTextBrush;
    ComPtr<ID2D1SolidColorBrush> plusTextBrush;
};
#endif

VideoCell::VideoCell(int cellId, QWidget* parent)
    : QWidget(parent), m_cellId(cellId)
{
    setMinimumSize(160, 120);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAcceptDrops(true);

#ifdef Q_OS_WIN
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    (void)winId();
#endif

    m_statusLabel = new QLabel(this);
    m_statusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_statusLabel->setAutoFillBackground(true);
    m_statusLabel->setAttribute(Qt::WA_StyledBackground, true);
    m_statusLabel->setStyleSheet(
        "QLabel { background-color: #000000; color: #dddddd;"
        "         font-size: 9px; padding-left: 4px; }");
    m_statusLabel->setGeometry(statusLabelRectFor(size()));
    m_statusLabel->hide();
}

VideoCell::~VideoCell() {
    discardRenderResources();
}

// ── public slots ──────────────────────────────────────────────

void VideoCell::activate(const QString& url) {
    m_active = true;
    m_url    = url;
    m_frame = {};
    m_baseStatus.clear();
    m_fpsText = "0.0 FPS";
    m_fpsFrameCount = 0;
    m_fpsTimer.restart();
    {
        QMutexLocker lock(&m_gpuFrameMutex);
        m_gpuFrame.release();
    }
    m_bitmapDirty = true;
#ifdef Q_OS_WIN
    // 이전 interop 실패 상태가 남아 있으면 스트림 시작 시 렌더 상태를 새로 만든다.
    if (m_renderState && m_renderState->gpuInteropDisabled)
        discardRenderResources();
#endif
    setStatus("연결 중...");
    update();
}

void VideoCell::deactivate() {
    m_active = false;
    m_url.clear();
    m_frame = {};
    m_baseStatus.clear();
    m_fpsText.clear();
    m_fpsFrameCount = 0;
    m_fpsTimer.invalidate();
    {
        QMutexLocker lock(&m_gpuFrameMutex);
        m_gpuFrame.release();
    }
    m_bitmapDirty = true;
#ifdef Q_OS_WIN
    // 비활성화 시 렌더 상태를 정리해 셀 단위 고착 상태를 다음 재생으로 넘기지 않는다.
    discardRenderResources();
#endif
    setStatus(QString());
    update();
}

void VideoCell::updateFrame(const QImage& frame) {
    {
        QMutexLocker lock(&m_gpuFrameMutex);
        m_gpuFrame.release();
    }

    if (isDirectRenderableFormat(frame.format())) {
        m_frame = frame;
    } else {
        m_frame = frame.convertToFormat(QImage::Format_ARGB32);
    }

    onFramePresented();
    m_bitmapDirty = true;
    update();
}

void VideoCell::updateGpuFrame(const cv::cuda::GpuMat& frame) {
#ifdef Q_OS_WIN
    if (frame.empty())
        return;

    cv::cuda::GpuMat gpuBgra;
    if (frame.type() == CV_8UC4) {
        gpuBgra = frame;
    } else if (frame.type() == CV_8UC3) {
        cv::cuda::cvtColor(frame, gpuBgra, cv::COLOR_BGR2BGRA);
    } else {
        return;
    }

    {
        QMutexLocker lock(&m_gpuFrameMutex);
        m_gpuFrame = gpuBgra;
    }

    // GPU 경로만 사용하므로 CPU 이미지 폴백은 유지하지 않는다.
    m_frame = {};

    onFramePresented();
    m_bitmapDirty = true;
    update();
#else
    Q_UNUSED(frame);
#endif
}

void VideoCell::setStatus(const QString& status) {
    m_baseStatus = status.trimmed();
    refreshStatusLabel();
}

void VideoCell::onFramePresented() {
    if (!m_active)
        return;

    if (!m_fpsTimer.isValid()) {
        m_fpsTimer.start();
        m_fpsFrameCount = 0;
        return;
    }

    ++m_fpsFrameCount;
    const qint64 elapsedMs = m_fpsTimer.elapsed();
    if (elapsedMs < 1000)
        return;

    const double fps = (static_cast<double>(m_fpsFrameCount) * 1000.0) /
                       static_cast<double>(elapsedMs);
    m_fpsText = QString("%1 FPS").arg(fps, 0, 'f', 1);
    m_fpsFrameCount = 0;
    m_fpsTimer.restart();
    refreshStatusLabel();
}

void VideoCell::refreshStatusLabel() {
    QString text;
    if (!m_baseStatus.isEmpty() && !m_fpsText.isEmpty()) {
        text = QString(" %1  |  %2").arg(m_baseStatus, m_fpsText);
    } else if (!m_baseStatus.isEmpty()) {
        text = " " + m_baseStatus;
    } else if (!m_fpsText.isEmpty()) {
        text = " " + m_fpsText;
    }

    if (text.isEmpty()) {
        m_statusLabel->clear();
        m_statusLabel->hide();
        update(statusRectFor(size()));
        return;
    }

    m_statusLabel->setText(text);
    m_statusLabel->setGeometry(statusLabelRectFor(size()));
    m_statusLabel->show();
    m_statusLabel->raise();
    update(statusRectFor(size()));
}

// ── events ────────────────────────────────────────────────────

void VideoCell::dragEnterEvent(QDragEnterEvent* ev) {
    const auto* md = ev->mimeData();
    if (md->hasFormat(CELL_DRAG_MIME) || md->hasText() || md->hasUrls()) {
        ev->acceptProposedAction();
        m_dragHover = true;
        update();
        return;
    }
    ev->ignore();
}

void VideoCell::dragMoveEvent(QDragMoveEvent* ev) {
    const auto* md = ev->mimeData();
    if (md->hasFormat(CELL_DRAG_MIME) || md->hasText() || md->hasUrls()) {
        ev->acceptProposedAction();
        return;
    }
    ev->ignore();
}

void VideoCell::dragLeaveEvent(QDragLeaveEvent* ev) {
    Q_UNUSED(ev);
    if (m_dragHover) {
        m_dragHover = false;
        update();
    }
}

void VideoCell::dropEvent(QDropEvent* ev) {
    m_dragHover = false;
    update();

    const auto* md = ev->mimeData();

    if (md->hasFormat(CELL_DRAG_MIME)) {
        bool ok = false;
        const int fromCellId = QString::fromUtf8(md->data(CELL_DRAG_MIME)).toInt(&ok);
        if (ok && fromCellId != m_cellId) {
            emit moveRequested(fromCellId, m_cellId);
            ev->acceptProposedAction();
            return;
        }
    }

    QString url;
    if (md->hasText()) {
        url = md->text().trimmed();
    } else if (md->hasUrls() && !md->urls().isEmpty()) {
        url = md->urls().first().toString().trimmed();
    }

    if (!url.isEmpty()) {
        bool hasCameraId = false;
        int cameraId = 0;
        if (md->hasFormat(CAMERA_ID_MIME)) {
            bool ok = false;
            cameraId = QString::fromUtf8(md->data(CAMERA_ID_MIME)).toInt(&ok);
            hasCameraId = ok && cameraId > 0;
        }

        if (hasCameraId) {
            emit addRequestedByCamera(m_cellId, cameraId, url);
        } else {
            emit addRequested(m_cellId, url);
        }
        ev->acceptProposedAction();
        return;
    }
    ev->ignore();
}

void VideoCell::mousePressEvent(QMouseEvent* ev) {
    if (ev->button() == Qt::LeftButton)
        m_dragStartPos = ev->pos();
    QWidget::mousePressEvent(ev);
}

void VideoCell::mouseMoveEvent(QMouseEvent* ev) {
    if (!(ev->buttons() & Qt::LeftButton) || !m_active || m_url.isEmpty()) {
        QWidget::mouseMoveEvent(ev);
        return;
    }

    if ((ev->pos() - m_dragStartPos).manhattanLength() < QApplication::startDragDistance()) {
        QWidget::mouseMoveEvent(ev);
        return;
    }

    auto* mime = new QMimeData();
    mime->setText(m_url);
    mime->setData(CELL_DRAG_MIME, QByteArray::number(m_cellId));

    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->exec(Qt::MoveAction);
}

void VideoCell::resizeEvent(QResizeEvent* ev) {
    QWidget::resizeEvent(ev);
#ifdef Q_OS_WIN
    if (m_renderState && m_renderState->swapChain) {
        if (m_renderState->d2dContext)
            m_renderState->d2dContext->SetTarget(nullptr);
        m_renderState->targetBitmap.Reset();
        const HRESULT hr = m_renderState->swapChain->ResizeBuffers(
            0,
            static_cast<UINT>(qMax(1, ev->size().width())),
            static_cast<UINT>(qMax(1, ev->size().height())),
            DXGI_FORMAT_B8G8R8A8_UNORM,
            0);
        if (FAILED(hr)) {
            qWarning() << "[VideoCell][D3D] cell" << (m_cellId + 1)
                       << "ResizeBuffers failed. Recreating render resources.";
            discardRenderResources();
        } else {
            m_bitmapDirty = true;
        }
    }
#endif
    m_statusLabel->setGeometry(statusLabelRectFor(ev->size()));
}

QPaintEngine* VideoCell::paintEngine() const {
#ifdef Q_OS_WIN
    return nullptr;
#else
    return QWidget::paintEngine();
#endif
}

void VideoCell::discardRenderResources() {
#ifdef Q_OS_WIN
    if (m_renderState && m_renderState->cudaFrameResource) {
        cudaGraphicsUnregisterResource(m_renderState->cudaFrameResource);
        m_renderState->cudaFrameResource = nullptr;
    }
    m_renderState.reset();
#endif
}

#ifdef Q_OS_WIN
void VideoCell::updateRenderBitmap() {
    if (!m_bitmapDirty || !m_renderState || !m_renderState->d2dContext)
        return;

    m_bitmapDirty = false;

    cv::cuda::GpuMat gpuFrame;
    {
        QMutexLocker lock(&m_gpuFrameMutex);
        gpuFrame = m_gpuFrame;
    }

    auto disableInterop = [this]() {
        if (!m_renderState)
            return;

        m_renderState->gpuInteropDisabled = true;
        if (m_renderState->cudaFrameResource) {
            cudaGraphicsUnregisterResource(m_renderState->cudaFrameResource);
            m_renderState->cudaFrameResource = nullptr;
        }
        m_renderState->frameBitmap.Reset();
        m_renderState->gpuFrameTexture.Reset();
        m_renderState->gpuFrameSurface.Reset();
        m_renderState->gpuFrameWidth = 0;
        m_renderState->gpuFrameHeight = 0;

        const int failures = ++m_renderState->interopFailureCount;
        qint64 delayMs = kInteropRetryBaseMs;
        for (int i = 1; i < failures && delayMs < kInteropRetryMaxMs; ++i)
            delayMs = qMin(kInteropRetryMaxMs, delayMs * 2);

        m_renderState->interopRetryAtMs = QDateTime::currentMSecsSinceEpoch() + delayMs;
        qWarning() << "[VideoCell][Interop] cell" << (m_cellId + 1)
                   << "interop temporarily disabled for" << delayMs << "ms.";
    };

    const bool interopDisabledByEnv = g_gpuInteropGloballyDisabled.load(std::memory_order_acquire);
    if (m_renderState->gpuInteropDisabled && !interopDisabledByEnv) {
        if (QDateTime::currentMSecsSinceEpoch() >= m_renderState->interopRetryAtMs) {
            m_renderState->gpuInteropDisabled = false;
            qInfo() << "[VideoCell][Interop] cell" << (m_cellId + 1)
                    << "retrying CUDA-D3D interop.";
        }
    }

    if (!gpuFrame.empty() && gpuFrame.type() == CV_8UC4 && m_renderState->d3dDevice &&
        !m_renderState->gpuInteropDisabled &&
        !interopDisabledByEnv) {
        bool interopOk = true;

        const int width = gpuFrame.cols;
        const int height = gpuFrame.rows;
        if (!m_renderState->gpuFrameTexture ||
            m_renderState->gpuFrameWidth != width ||
            m_renderState->gpuFrameHeight != height) {
            if (m_renderState->cudaFrameResource) {
                cudaGraphicsUnregisterResource(m_renderState->cudaFrameResource);
                m_renderState->cudaFrameResource = nullptr;
            }

            m_renderState->frameBitmap.Reset();
            m_renderState->gpuFrameTexture.Reset();
            m_renderState->gpuFrameSurface.Reset();

            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = static_cast<UINT>(width);
            desc.Height = static_cast<UINT>(height);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

            if (FAILED(m_renderState->d3dDevice->CreateTexture2D(
                    &desc, nullptr, m_renderState->gpuFrameTexture.GetAddressOf()))) {
                interopOk = false;
                logInteropFailure(m_cellId, "CreateTexture2D");
            }

            if (interopOk && FAILED(m_renderState->gpuFrameTexture.As(&m_renderState->gpuFrameSurface))) {
                interopOk = false;
                logInteropFailure(m_cellId, "QueryInterface IDXGISurface");
            }

            const auto props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            if (interopOk && FAILED(m_renderState->d2dContext->CreateBitmapFromDxgiSurface(
                    m_renderState->gpuFrameSurface.Get(),
                    &props,
                    m_renderState->frameBitmap.ReleaseAndGetAddressOf()))) {
                interopOk = false;
                logInteropFailure(m_cellId, "CreateBitmapFromDxgiSurface");
            }

            if (interopOk) {
                const auto registerResult = cudaGraphicsD3D11RegisterResource(
                    &m_renderState->cudaFrameResource,
                    m_renderState->gpuFrameTexture.Get(),
                    cudaGraphicsRegisterFlagsNone);
                if (registerResult != cudaSuccess) {
                    m_renderState->cudaFrameResource = nullptr;
                    interopOk = false;
                    logInteropFailure(m_cellId, "cudaGraphicsD3D11RegisterResource", registerResult);
                }
            }

            if (!interopOk) {
                m_renderState->cudaFrameResource = nullptr;
            }

            if (interopOk) {
                m_renderState->gpuFrameWidth = width;
                m_renderState->gpuFrameHeight = height;
            }
        }

        if (interopOk && m_renderState->cudaFrameResource) {
            const auto mapResult = cudaGraphicsMapResources(1, &m_renderState->cudaFrameResource, nullptr);
            if (mapResult == cudaSuccess) {
                cudaArray_t mappedArray = nullptr;
                if (cudaGraphicsSubResourceGetMappedArray(
                        &mappedArray, m_renderState->cudaFrameResource, 0, 0) == cudaSuccess) {
                    const auto copyResult = cudaMemcpy2DToArray(
                        mappedArray,
                        0,
                        0,
                        gpuFrame.ptr(),
                        gpuFrame.step,
                        static_cast<size_t>(gpuFrame.cols) * 4,
                        static_cast<size_t>(gpuFrame.rows),
                        cudaMemcpyDeviceToDevice);
                    if (copyResult != cudaSuccess) {
                        interopOk = false;
                        logInteropFailure(m_cellId, "cudaMemcpy2DToArray", copyResult);
                    }
                } else {
                    interopOk = false;
                    logInteropFailure(m_cellId, "cudaGraphicsSubResourceGetMappedArray");
                }
                cudaGraphicsUnmapResources(1, &m_renderState->cudaFrameResource, nullptr);
            } else {
                interopOk = false;
                logInteropFailure(m_cellId, "cudaGraphicsMapResources", mapResult);
            }
        } else {
            interopOk = false;
        }

        if (interopOk) {
            m_renderState->interopFailureCount = 0;
            m_renderState->interopRetryAtMs = 0;
            return;
        }

        // interop 실패 시 전역적으로 interop를 끈다.
        disableInterop();
    }

    m_renderState->frameBitmap.Reset();

    QImage cpuFallbackFrame;
    const QImage* sourceFrame = &m_frame;
    if (sourceFrame->isNull() && !gpuFrame.empty() && gpuFrame.type() == CV_8UC4) {
        cv::Mat cpuFrame;
        gpuFrame.download(cpuFrame);
        if (!cpuFrame.empty()) {
            // GpuMat 메모리 생명주기와 분리하기 위해 copy()로 독립 버퍼를 만든다.
            const QImage wrapped(
                cpuFrame.data,
                cpuFrame.cols,
                cpuFrame.rows,
                static_cast<qsizetype>(cpuFrame.step),
                QImage::Format_ARGB32);
            cpuFallbackFrame = wrapped.copy();
            sourceFrame = &cpuFallbackFrame;
        }
    }

    if (sourceFrame->isNull())
        return;

    ID2D1Bitmap1* bitmap = nullptr;
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

    const HRESULT hr = m_renderState->d2dContext->CreateBitmap(
        D2D1::SizeU(static_cast<UINT32>(sourceFrame->width()), static_cast<UINT32>(sourceFrame->height())),
        sourceFrame->constBits(),
        static_cast<UINT32>(sourceFrame->bytesPerLine()),
        props,
        &bitmap);

    if (SUCCEEDED(hr))
        m_renderState->frameBitmap.Attach(bitmap);
}

void VideoCell::renderDirectX() {
    if (size().isEmpty())
        return;

    if (!m_renderState)
        m_renderState = std::make_unique<RenderState>();

    if (!m_renderState->factory) {
        ID2D1Factory1* factory = nullptr;
        if (FAILED(D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED,
                __uuidof(ID2D1Factory1),
                nullptr,
                reinterpret_cast<void**>(&factory))))
            return;
        m_renderState->factory.Attach(factory);
    }

    if (!m_renderState->dwriteFactory) {
        IDWriteFactory* dwriteFactory = nullptr;
        const HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&dwriteFactory));
        if (FAILED(hr))
            return;
        m_renderState->dwriteFactory.Attach(dwriteFactory);
    }

    if (!m_renderState->d3dDevice) {
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };

        ID3D11Device* d3dDevice = nullptr;
        ID3D11DeviceContext* d3dContext = nullptr;
        D3D_FEATURE_LEVEL createdLevel{};
        if (SUCCEEDED(D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels,
                ARRAYSIZE(levels),
                D3D11_SDK_VERSION,
                &d3dDevice,
                &createdLevel,
                &d3dContext))) {
            m_renderState->d3dDevice.Attach(d3dDevice);
            m_renderState->d3dContext.Attach(d3dContext);
        }
    }

    if (!m_renderState->d2dDevice || !m_renderState->d2dContext) {
        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(m_renderState->d3dDevice.As(&dxgiDevice)))
            return;

        if (!m_renderState->d2dDevice && FAILED(m_renderState->factory->CreateDevice(
                dxgiDevice.Get(), m_renderState->d2dDevice.GetAddressOf()))) {
            return;
        }

        if (!m_renderState->d2dContext && FAILED(m_renderState->d2dDevice->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                m_renderState->d2dContext.GetAddressOf()))) {
            return;
        }
    }

    if (!m_renderState->swapChain) {
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        if (!hwnd)
            return;

        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> dxgiAdapter;
        ComPtr<IDXGIFactory2> dxgiFactory;
        if (FAILED(m_renderState->d3dDevice.As(&dxgiDevice)) ||
            FAILED(dxgiDevice->GetAdapter(dxgiAdapter.GetAddressOf())) ||
            FAILED(dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory)))) {
            return;
        }

        const auto pixelSize = nativeClientPixelSize(hwnd);
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = pixelSize.width;
        desc.Height = pixelSize.height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        if (FAILED(dxgiFactory->CreateSwapChainForHwnd(
                m_renderState->d3dDevice.Get(),
                hwnd,
                &desc,
                nullptr,
                nullptr,
                m_renderState->swapChain.GetAddressOf()))) {
            return;
        }

        dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    }

    if (!m_renderState->targetBitmap) {
        ComPtr<IDXGISurface> backBuffer;
        if (FAILED(m_renderState->swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
            return;

        const auto targetProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(m_renderState->d2dContext->CreateBitmapFromDxgiSurface(
                backBuffer.Get(),
                &targetProps,
                m_renderState->targetBitmap.GetAddressOf()))) {
            return;
        }

        m_renderState->d2dContext->SetTarget(m_renderState->targetBitmap.Get());

        ID2D1SolidColorBrush* brush = nullptr;
        m_renderState->d2dContext->CreateSolidColorBrush(toD2D(QColor(0, 120, 215)), &brush);
        m_renderState->activeBorderBrush.Attach(brush);

        brush = nullptr;
        m_renderState->d2dContext->CreateSolidColorBrush(toD2D(QColor(45, 45, 45)), &brush);
        m_renderState->inactiveBorderBrush.Attach(brush);

        brush = nullptr;
        m_renderState->d2dContext->CreateSolidColorBrush(toD2D(QColor(76, 175, 80)), &brush);
        m_renderState->dragBorderBrush.Attach(brush);

        brush = nullptr;
        m_renderState->d2dContext->CreateSolidColorBrush(toD2D(Qt::black), &brush);
        m_renderState->placeholderBrush.Attach(brush);

        brush = nullptr;
        m_renderState->d2dContext->CreateSolidColorBrush(toD2D(QColor(80, 80, 80)), &brush);
        m_renderState->channelTextBrush.Attach(brush);

        brush = nullptr;
        m_renderState->d2dContext->CreateSolidColorBrush(toD2D(QColor(55, 55, 55)), &brush);
        m_renderState->plusTextBrush.Attach(brush);
    }

    updateRenderBitmap();

    auto* rt = m_renderState->d2dContext.Get();
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());

    const D2D1_SIZE_F sizeF = rt->GetSize();
    const FLOAT statusTop = qMax(0.0f, sizeF.height - static_cast<FLOAT>(STATUS_H));
    const D2D1_RECT_F videoRect = D2D1::RectF(0.f, 0.f, sizeF.width, statusTop);
    const D2D1_RECT_F statusRect = D2D1::RectF(0.f, statusTop, sizeF.width, sizeF.height);

    rt->Clear(toD2D(Qt::black));
    rt->FillRectangle(statusRect, m_renderState->placeholderBrush.Get());

    if (m_active && m_renderState->frameBitmap) {
        rt->FillRectangle(videoRect, m_renderState->placeholderBrush.Get());

        const auto bitmapSize = m_renderState->frameBitmap->GetSize();
        const QRectF destRect = aspectFitRect(
            QSizeF(bitmapSize.width, bitmapSize.height),
            QSizeF(sizeF.width, qMax(1.0f, statusTop)));

        rt->DrawBitmap(
            m_renderState->frameBitmap.Get(),
            D2D1::RectF(static_cast<FLOAT>(destRect.left()),
                        static_cast<FLOAT>(destRect.top()),
                        static_cast<FLOAT>(destRect.right()),
                        static_cast<FLOAT>(destRect.bottom())),
            1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        rt->FillRectangle(videoRect, m_renderState->placeholderBrush.Get());

        const QString channel = QString("CH %1").arg(m_cellId + 1);
        const std::wstring channelText = channel.toStdWString();
        const std::wstring plusText = L"+";

        const FLOAT chFontSize = qMax(10.0f, sizeF.width / 14.0f);
        const FLOAT plusFontSize = qMax(20.0f, sizeF.width / 7.0f);

        ComPtr<IDWriteTextFormat> channelFormat;
        if (SUCCEEDED(m_renderState->dwriteFactory->CreateTextFormat(
                L"Segoe UI", nullptr,
                DWRITE_FONT_WEIGHT_BOLD,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                chFontSize,
                L"",
                channelFormat.GetAddressOf()))) {
            channelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            channelFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            rt->DrawTextW(
                channelText.c_str(),
                static_cast<UINT32>(channelText.size()),
                channelFormat.Get(),
                D2D1::RectF(0.f, 0.f, sizeF.width, qMax(1.0f, statusTop - (statusTop / 4.0f))),
                m_renderState->channelTextBrush.Get());
        }

        ComPtr<IDWriteTextFormat> plusFormat;
        if (SUCCEEDED(m_renderState->dwriteFactory->CreateTextFormat(
                L"Segoe UI", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                plusFontSize,
                L"",
                plusFormat.GetAddressOf()))) {
            plusFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            plusFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            rt->DrawTextW(
                plusText.c_str(),
                static_cast<UINT32>(plusText.size()),
                plusFormat.Get(),
                D2D1::RectF(0.f, statusTop / 5.0f, sizeF.width, statusTop),
                m_renderState->plusTextBrush.Get());
        }
    }

    const FLOAT borderWidth = m_active ? 2.0f : 1.0f;
    auto* borderBrush = m_active ? m_renderState->activeBorderBrush.Get()
                                 : m_renderState->inactiveBorderBrush.Get();
    rt->FillRectangle(D2D1::RectF(0.f, 0.f, sizeF.width, borderWidth), borderBrush);
    rt->FillRectangle(D2D1::RectF(0.f, sizeF.height - borderWidth, sizeF.width, sizeF.height), borderBrush);
    rt->FillRectangle(D2D1::RectF(0.f, 0.f, borderWidth, sizeF.height), borderBrush);
    rt->FillRectangle(D2D1::RectF(sizeF.width - borderWidth, 0.f, sizeF.width, sizeF.height), borderBrush);

    if (m_dragHover) {
        auto* dragBrush = m_renderState->dragBorderBrush.Get();
        constexpr FLOAT dragWidth = 2.0f;
        rt->FillRectangle(D2D1::RectF(0.f, 0.f, sizeF.width, dragWidth), dragBrush);
        rt->FillRectangle(D2D1::RectF(0.f, sizeF.height - dragWidth, sizeF.width, sizeF.height), dragBrush);
        rt->FillRectangle(D2D1::RectF(0.f, 0.f, dragWidth, sizeF.height), dragBrush);
        rt->FillRectangle(D2D1::RectF(sizeF.width - dragWidth, 0.f, sizeF.width, sizeF.height), dragBrush);
    }

    const HRESULT hr = rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        m_bitmapDirty = true;
        discardRenderResources();
        return;
    }

    if (FAILED(m_renderState->swapChain->Present(0, 0))) {
        m_bitmapDirty = true;
        discardRenderResources();
    }
}
#endif

void VideoCell::paintEvent(QPaintEvent*) {
#ifdef Q_OS_WIN
    renderDirectX();
#else
    QPainter p(this);
    const QRect videoRect = videoRectFor(size());
    const QRect statusRect = statusRectFor(size());

    if (m_active && !m_frame.isNull()) {
        p.fillRect(videoRect, Qt::black);
        const QRectF dest = aspectFitRect(m_frame.size(), videoRect.size());
        const QRectF translated = dest.translated(videoRect.topLeft());
        p.drawImage(translated, m_frame);
    } else {
        p.fillRect(videoRect, Qt::black);

        QFont f;
        f.setPointSize(qMax(7, width() / 14));
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(80, 80, 80));
        p.drawText(videoRect.adjusted(0, 0, 0, -videoRect.height() / 4),
                   Qt::AlignCenter,
                   QString("CH %1").arg(m_cellId + 1));

        QFont pf;
        pf.setPointSize(qMax(14, width() / 7));
        p.setFont(pf);
        p.setPen(QColor(55, 55, 55));
        p.drawText(videoRect.adjusted(0, videoRect.height() / 5, 0, 0),
                   Qt::AlignCenter, "+");
    }

    p.fillRect(statusRect, Qt::black);

    const QColor border = m_active ? QColor(0, 120, 215) : QColor(45, 45, 45);
    const int bw = m_active ? 2 : 1;
    const QRect r = rect();
    if (r.width() > 0 && r.height() > 0) {
        p.fillRect(r.left(), r.top(), r.width(), bw, border);                       // top
        p.fillRect(r.left(), r.bottom() - bw + 1, r.width(), bw, border);           // bottom
        p.fillRect(r.left(), r.top(), bw, r.height(), border);                       // left
        p.fillRect(r.right() - bw + 1, r.top(), bw, r.height(), border);            // right
    }

    if (m_dragHover) {
        p.fillRect(r.left(), r.top(), r.width(), 2, QColor(76, 175, 80));
        p.fillRect(r.left(), r.bottom() - 1, r.width(), 2, QColor(76, 175, 80));
        p.fillRect(r.left(), r.top(), 2, r.height(), QColor(76, 175, 80));
        p.fillRect(r.right() - 1, r.top(), 2, r.height(), QColor(76, 175, 80));
    }
#endif
}

void VideoCell::mouseDoubleClickEvent(QMouseEvent*) {
    if (!m_active) showAddDialog();
}

void VideoCell::contextMenuEvent(QContextMenuEvent* ev) {
    // Native child HWND(VideoCell) 대신 top-level window를 팝업 소유자로 사용한다.
    QWidget* popupParent = window();
    if (!popupParent)
        popupParent = this;

    QMenu menu(popupParent);
    if (!m_active) {
        menu.addAction("스트림 추가...", this, &VideoCell::showAddDialog);
    } else {
        auto* info = menu.addAction(m_url);
        info->setEnabled(false);
        menu.addSeparator();
        menu.addAction("스트림 제거", this,
                       [this]() { emit removeRequested(m_cellId); });
    }
    menu.exec(ev->globalPos());
}

void VideoCell::showAddDialog() {
    // 입력 다이얼로그도 top-level window를 부모로 지정해 native popup 경고를 피한다.
    QWidget* dialogParent = window();
    if (!dialogParent)
        dialogParent = this;

    QInputDialog dialog(dialogParent);
    dialog.setInputMode(QInputDialog::TextInput);
    dialog.setWindowTitle(QString("채널 %1 - 스트림 추가").arg(m_cellId + 1));
    dialog.setLabelText("RTSP URL:");
    dialog.setTextEchoMode(QLineEdit::Normal);
    dialog.setTextValue("rtsp://");

    // show 전에 크기를 확정해 Windows의 재배치 경고 가능성을 낮춘다.
    dialog.ensurePolished();
    const QSize targetSize = dialog.sizeHint().expandedTo(dialog.minimumSizeHint());
    dialog.resize(targetSize);

    if (dialog.exec() == QDialog::Accepted) {
        const QString url = dialog.textValue().trimmed();
        if (!url.isEmpty())
            emit addRequested(m_cellId, url);
    }
}

