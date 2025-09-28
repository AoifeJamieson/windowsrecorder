#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <iostream>
#include <vector> // Used for the cursor buffer, though not currently implemented

// Media Foundation Headers
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

// Link necessary libraries
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfreadwrite.lib")

// --- Helper Functions ---

// Safely releases a COM interface pointer and sets it to nullptr.
template <class T> void SafeRelease(T** ppT)
{
    if (*ppT)
    {
        (*ppT)->Release();
        *ppT = nullptr;
    }
}


//======================================================================================
// Recorder Class
// Encapsulates all the logic for initializing DirectX, capturing the screen,
// and encoding it to a video file.
//======================================================================================
class Recorder
{
public:
    // Constructor: Initializes all COM pointers to null.
    Recorder() :
        m_pDevice(nullptr),
        m_pContext(nullptr),
        m_pDuplication(nullptr)
    {
    }

    // Destructor: Ensures all resources are released.
    ~Recorder()
    {
        // The SafeRelease helper handles null pointers, so this is safe.
        SafeRelease(&m_pDuplication);
        SafeRelease(&m_pContext);
        SafeRelease(&m_pDevice);
    }

    // Public methods
    HRESULT Initialize();
    HRESULT Record();

private:
    // Private helper methods
    HRESULT GrabFrameAndCreateSample(IMFSample** ppSample);

    // Private member variables for DirectX state
    ID3D11Device* m_pDevice;
    ID3D11DeviceContext* m_pContext;
    IDXGIOutputDuplication* m_pDuplication;
};

// --- Main Application Entry Point ---
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    // Attach a console so we can see the verbose std::cout output.
    // This is purely for debugging and can be removed for a final release.
    AllocConsole();
    FILE* fDummy;
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    freopen_s(&fDummy, "CONOUT$", "w", stderr);

    // Initialize the COM library for the Multi-Threaded Apartment, and Media Foundation.
    // MTA is required for this synchronous, console-like application model to avoid deadlocks.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    std::cout << "--- Starting Application ---" << std::endl;

    // Create an invisible window. Its existence gives our application the proper
    // desktop session context required by the Desktop Duplication API to succeed.
    WNDCLASS wc = { 0 };
    const wchar_t CLASS_NAME[] = L"ScreenRecordWindowClass";
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClass(&wc);
    HWND hWnd = CreateWindowEx(0, CLASS_NAME, L"Screen Recorder", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);

    // Create an instance of our Recorder class
    Recorder rec;

    // Initialize the recorder (finds monitor, creates D3D device, sets up duplication)
    HRESULT hr = rec.Initialize();

    if (FAILED(hr))
    {
        MessageBox(nullptr, L"Failed to initialize DXGI for screen capture.", L"Error", MB_OK | MB_ICONERROR);
    }
    else
    {
        // If initialization succeeded, start the recording process.
        std::cout << "\n--- Starting Capture ---" << std::endl;
        hr = rec.Record();
        if (SUCCEEDED(hr))
        {
            MessageBox(nullptr, L"Successfully recorded 5 seconds of video to output.mp4!", L"Success", MB_OK);
        }
        else
        {
            MessageBox(nullptr, L"Failed to record video.", L"Capture Error", MB_OK | MB_ICONERROR);
        }
    }

    // The Recorder's destructor will automatically be called here, cleaning up its resources.

    // Shut down Media Foundation and COM.
    MFShutdown();
    CoUninitialize();
    std::cout << "\n--- Application Exiting ---" << std::endl;
    return 0;
}


//======================================================================================
// Recorder Class Method Implementations
//======================================================================================

//--------------------------------------------------------------------------------------
// [Recorder::Initialize]
// Finds the primary monitor and sets up the D3D11 device and Desktop Duplication API.
//--------------------------------------------------------------------------------------
HRESULT Recorder::Initialize()
{
    HRESULT hr = S_OK;

    IDXGIFactory1* pFactory = nullptr;
    IDXGIAdapter1* pAdapter = nullptr;

    // Create a DXGI Factory
    hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)(&pFactory));
    if (FAILED(hr))
    {
        return hr;
    }

    // --- Enumerate Adapters (Graphics Cards) ---
    // We'll iterate through all adapters until we find one that has an attached monitor.
    for (UINT i = 0; ; ++i)
    {
        // Get the next adapter
        hr = pFactory->EnumAdapters1(i, &pAdapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
        {
            // No more adapters, so we break the loop
            break;
        }
        if (FAILED(hr))
        {
            continue; // Failed to get adapter, try the next one
        }

        // --- Enumerate Outputs (Monitors) ---
        // Iterate through all monitors attached to this adapter.
        for (UINT j = 0; ; ++j)
        {
            IDXGIOutput* pOutput = nullptr;
            hr = pAdapter->EnumOutputs(j, &pOutput);
            if (hr == DXGI_ERROR_NOT_FOUND)
            {
                // No more outputs on this adapter
                break;
            }
            if (FAILED(hr))
            {
                continue; // Failed to get output, try the next one
            }

            // Check if the monitor is attached to the desktop
            DXGI_OUTPUT_DESC desc;
            pOutput->GetDesc(&desc);
            if (desc.AttachedToDesktop)
            {
                IDXGIOutput1* pOutput1 = nullptr;
                hr = pOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&pOutput1);
                if (SUCCEEDED(hr))
                {
                    // If we found a monitor, NOW we create the D3D11 Device from its adapter.
                    hr = D3D11CreateDevice(pAdapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &m_pDevice, NULL, &m_pContext);
                    if (SUCCEEDED(hr))
                    {
                        // And finally, create the duplication interface from the device.
                        hr = pOutput1->DuplicateOutput(m_pDevice, &m_pDuplication);
                        if (SUCCEEDED(hr))
                        {
                            // Success! We have found a working setup.
                            std::cout << "Successfully created duplication for an attached monitor!" << std::endl;
                            SafeRelease(&pOutput1);
                            SafeRelease(&pOutput);
                            SafeRelease(&pAdapter);
                            SafeRelease(&pFactory);
                            return S_OK;
                        }
                        // Cleanup failed device creation
                        SafeRelease(&m_pDevice);
                        SafeRelease(&m_pContext);
                    }
                }
                SafeRelease(&pOutput1);
            }
            SafeRelease(&pOutput);
        }
        SafeRelease(&pAdapter);
    }

    SafeRelease(&pFactory);
    // If we get here, we never found a suitable monitor.
    return E_FAIL;
}

//--------------------------------------------------------------------------------------
// [Recorder::Record]
// Configures and runs the main video encoding loop.
//--------------------------------------------------------------------------------------
HRESULT Recorder::Record()
{
    HRESULT hr = S_OK;
    IMFSinkWriter* pSinkWriter = nullptr;
    IMFDXGIDeviceManager* pDeviceManager = nullptr;
    IMFAttributes* pAttributes = nullptr;

    // This do-while(false) loop is a C++-friendly replacement for goto statements.
    // If any step fails, we can 'break' to the cleanup section at the end.
    do
    {
        // --- Define Video Parameters ---
        const UINT32 VIDEO_FPS = 30;
        const UINT32 VIDEO_BIT_RATE = 8000000; // 8 Mbps
        const UINT64 VIDEO_FRAME_DURATION = 10 * 1000 * 1000 / VIDEO_FPS;
        LONGLONG rtStart = 0; // Running timestamp

        // Get the screen dimensions from the duplication description
        DXGI_OUTDUPL_DESC duplDesc;
        m_pDuplication->GetDesc(&duplDesc);
        const UINT32 VIDEO_WIDTH = duplDesc.ModeDesc.Width;
        const UINT32 VIDEO_HEIGHT = duplDesc.ModeDesc.Height;

        // --- Configure the Sink Writer ---

        // 1. Create the DXGI Device Manager. This is the crucial link that allows the
        //    Sink Writer's internal components (like the color converter) to use our GPU.
        UINT resetToken;
        hr = MFCreateDXGIDeviceManager(&resetToken, &pDeviceManager);
        if (FAILED(hr)) break;
        hr = pDeviceManager->ResetDevice(m_pDevice, resetToken);
        if (FAILED(hr)) break;

        // Create an attribute store to hold the device manager pointer.
        hr = MFCreateAttributes(&pAttributes, 1);
        if (FAILED(hr)) break;
        hr = pAttributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, pDeviceManager);
        if (FAILED(hr)) break;

        // 2. Create the Sink Writer, passing in the hardware attributes.
        std::cout << "Configuring Sink Writer for " << VIDEO_WIDTH << "x" << VIDEO_HEIGHT << " @ " << VIDEO_FPS << " FPS" << std::endl;
        hr = MFCreateSinkWriterFromURL(L"output.mp4", nullptr, pAttributes, &pSinkWriter);
        if (FAILED(hr)) break;

        // 3. Configure the Output Stream (what we want the final file to look like)
        DWORD streamIndex;
        IMFMediaType* pMediaTypeOut = nullptr;
        hr = MFCreateMediaType(&pMediaTypeOut);
        if (SUCCEEDED(hr))
        {
            hr = pMediaTypeOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            if (SUCCEEDED(hr)) hr = pMediaTypeOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264); // H.264 video
            if (SUCCEEDED(hr)) hr = pMediaTypeOut->SetUINT32(MF_MT_AVG_BITRATE, VIDEO_BIT_RATE);
            if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(pMediaTypeOut, MF_MT_FRAME_RATE, VIDEO_FPS, 1);
            if (SUCCEEDED(hr)) hr = MFSetAttributeSize(pMediaTypeOut, MF_MT_FRAME_SIZE, VIDEO_WIDTH, VIDEO_HEIGHT);
            if (SUCCEEDED(hr)) hr = pMediaTypeOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            if (SUCCEEDED(hr)) hr = pSinkWriter->AddStream(pMediaTypeOut, &streamIndex);
        }
        SafeRelease(&pMediaTypeOut);
        if (FAILED(hr)) break;

        // 4. Configure the Input Stream (what we will be feeding the writer)
        IMFMediaType* pMediaTypeIn = nullptr;
        hr = MFCreateMediaType(&pMediaTypeIn);
        if (SUCCEEDED(hr))
        {
            hr = pMediaTypeIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            if (SUCCEEDED(hr)) hr = pMediaTypeIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32); // Uncompressed 32-bit RGB from our capture
            if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(pMediaTypeIn, MF_MT_FRAME_RATE, VIDEO_FPS, 1);
            if (SUCCEEDED(hr)) hr = MFSetAttributeSize(pMediaTypeIn, MF_MT_FRAME_SIZE, VIDEO_WIDTH, VIDEO_HEIGHT);
            if (SUCCEEDED(hr)) hr = pMediaTypeIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            if (SUCCEEDED(hr)) hr = pSinkWriter->SetInputMediaType(streamIndex, pMediaTypeIn, nullptr);
        }
        SafeRelease(&pMediaTypeIn);
        if (FAILED(hr)) break;

        // 5. Start the encoding session.
        hr = pSinkWriter->BeginWriting();
        if (FAILED(hr)) break;
        std::cout << "Sink Writer configured. Starting capture loop..." << std::endl;

        // --- Main Capture Loop ---
        for (int i = 0; i < (VIDEO_FPS * 5); ++i)
        {
            IMFSample* pSample = nullptr;
            hr = GrabFrameAndCreateSample(&pSample);

            if (hr == S_FALSE) {
                // S_FALSE is our custom signal for a non-fatal timeout.
                std::cout << "Skipping frame " << i << " due to timeout." << std::endl;
                hr = S_OK; // Reset HR so we don't treat it as a failure
                continue;
            }
            if (FAILED(hr)) {
                std::cerr << "Failed to grab frame. Exiting loop." << std::endl;
                break; // A real error occurred, exit the loop.
            }

            // Set the timestamp and duration for the frame.
            hr = pSample->SetSampleTime(rtStart);
            if (FAILED(hr)) { SafeRelease(&pSample); break; }
            hr = pSample->SetSampleDuration(VIDEO_FRAME_DURATION);
            if (FAILED(hr)) { SafeRelease(&pSample); break; }

            // Write the frame to the video file.
            hr = pSinkWriter->WriteSample(streamIndex, pSample);
            if (FAILED(hr)) { SafeRelease(&pSample); break; }

            SafeRelease(&pSample);
            std::cout << "Wrote frame " << i << std::endl;
            rtStart += VIDEO_FRAME_DURATION; // Increment the timestamp for the next frame
        }
        if (FAILED(hr)) break;

        std::cout << "Capture loop finished." << std::endl;

    } while (false);

    // --- Finalize and Cleanup ---
    if (pSinkWriter)
    {
        std::cout << "Finalizing video file..." << std::endl;
        HRESULT finalizeHr = pSinkWriter->Finalize();
        // If the main loop succeeded but finalize failed, report the finalize error.
        if (SUCCEEDED(hr) && FAILED(finalizeHr))
        {
            hr = finalizeHr;
        }
    }

    SafeRelease(&pSinkWriter);
    SafeRelease(&pDeviceManager);
    SafeRelease(&pAttributes);

    if (FAILED(hr))
    {
        std::cerr << "An error occurred during recording. HRESULT: 0x" << std::hex << hr << std::endl;
    }
    return hr;
}


//--------------------------------------------------------------------------------------
// [Recorder::GrabFrameAndCreateSample]
// Captures a single frame, copies it to a CPU buffer, and creates an IMFSample.
// This is the core logic that was debugged and proven to be reliable.
//--------------------------------------------------------------------------------------
HRESULT Recorder::GrabFrameAndCreateSample(IMFSample** ppSample)
{
    HRESULT hr = S_OK;
    IDXGIResource* pDesktopResource = nullptr;
    ID3D11Texture2D* pDesktopTexture = nullptr;
    ID3D11Texture2D* pStagingTexture = nullptr;
    IMFMediaBuffer* pBuffer = nullptr;
    *ppSample = nullptr;

    do {
        // 1. Acquire a new frame from the Desktop Duplication API.
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        hr = m_pDuplication->AcquireNextFrame(1000, &frameInfo, &pDesktopResource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // This is not a fatal error, just no screen updates. We signal this with S_FALSE.
            hr = S_FALSE;
            break;
        }
        if (FAILED(hr)) break;

        // Get the underlying ID3D11Texture2D from the DXGI resource.
        hr = pDesktopResource->QueryInterface(IID_PPV_ARGS(&pDesktopTexture));
        if (FAILED(hr)) break;

        // 2. Create a "staging" texture. This is a special texture that the CPU can read.
        D3D11_TEXTURE2D_DESC desc;
        pDesktopTexture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        hr = m_pDevice->CreateTexture2D(&desc, NULL, &pStagingTexture);
        if (FAILED(hr)) break;

        // 3. Copy the GPU's desktop image to the staging texture.
        m_pContext->CopyResource(pStagingTexture, pDesktopTexture);

        // 4. Force the GPU to finish the copy operation. This is the critical step that
        //    prevents the "black screen" race condition.
        m_pContext->Flush();

        // 5. Map the staging texture, which gives the CPU read access to its pixel data.
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = m_pContext->Map(pStagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) break;

        // 6. Create a Media Foundation memory buffer and copy the pixel data into it,
        //    flipping the image vertically and correcting for stride mismatch in the process.
        hr = MFCreateMemoryBuffer(desc.Height * desc.Width * 4, &pBuffer);
        if (SUCCEEDED(hr)) {
            BYTE* pDst = nullptr;
            pBuffer->Lock(&pDst, NULL, NULL);

            const UINT Bpp = 4; // Bytes per pixel
            const UINT rowWidthInBytes = desc.Width * Bpp;
            BYTE* pSrc = (BYTE*)mapped.pData + ((desc.Height - 1) * mapped.RowPitch);

            for (UINT y = 0; y < desc.Height; ++y) {
                memcpy(pDst, pSrc, rowWidthInBytes);
                pDst += rowWidthInBytes;
                pSrc -= mapped.RowPitch;
            }

            pBuffer->Unlock();
            pBuffer->SetCurrentLength(desc.Height * desc.Width * 4);
        }
        m_pContext->Unmap(pStagingTexture, 0);
        if (FAILED(hr)) break;

        // 7. Create the final IMFSample and attach the buffer.
        hr = MFCreateSample(ppSample);
        if (FAILED(hr)) break;

        hr = (*ppSample)->AddBuffer(pBuffer);

    } while (false);

    // --- Cleanup ---
    // This is always called, whether we succeeded or failed.
    if (m_pDuplication) {
        // We must release the frame, even if we failed to process it.
        m_pDuplication->ReleaseFrame();
    }
    SafeRelease(&pDesktopResource);
    SafeRelease(&pDesktopTexture);
    SafeRelease(&pStagingTexture);
    SafeRelease(&pBuffer);

    // If any step failed, ensure the output sample is null.
    if (FAILED(hr)) {
        SafeRelease(ppSample);
    }
    return hr;
}