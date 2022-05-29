#include <iostream>
#include <vector>
#include <windows.h>
#include <thread>
#include <queue>
#include <fstream>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "infer.h"
#include "utils.h"
#include "pipeline.h"
#include "define.h"

#include <tchar.h>
#include <strsafe.h>
#include "sample.h"

static NTSTATUS(__stdcall* NtDelayExecution)(BOOL Alertable, PLARGE_INTEGER DelayInterval) = (NTSTATUS(__stdcall*)(BOOL, PLARGE_INTEGER)) GetProcAddress(GetModuleHandle(L"ntdll.dll"), "NtDelayExecution");
static NTSTATUS(__stdcall* ZwSetTimerResolution)(IN ULONG RequestedResolution, IN BOOLEAN Set, OUT PULONG ActualResolution) = (NTSTATUS(__stdcall*)(ULONG, BOOLEAN, PULONG)) GetProcAddress(GetModuleHandle(L"ntdll.dll"), "ZwSetTimerResolution");

static void SleepShort(float milliseconds) {
    static bool once = true;
    if (once) {
        ULONG actualResolution;
        ZwSetTimerResolution(1, true, &actualResolution);
        once = false;
    }

    LARGE_INTEGER interval;
    interval.QuadPart = -1 * (int)(milliseconds * 10000.0f);
    NtDelayExecution(false, &interval);
}

#pragma comment(lib, "advapi32.lib")

#define SVCNAME TEXT("MicDriverService")

using namespace std;
using namespace Eigen;

Queue in_queue, out_queue;

Pipeline* pipeline;
bool control = true;
bool use_pipeline = true;
vector<float> buffer;

HANDLE device = INVALID_HANDLE_VALUE;

SERVICE_STATUS          gSvcStatus;
SERVICE_STATUS_HANDLE   gSvcStatusHandle;
HANDLE                  ghSvcStopEvent = NULL;

VOID SvcInstall(void);
VOID WINAPI SvcCtrlHandler(DWORD);
VOID WINAPI SvcMain(DWORD, LPTSTR*);

VOID ReportSvcStatus(DWORD, DWORD, DWORD);
VOID SvcInit(DWORD, LPTSTR*);
VOID SvcReportEvent(LPTSTR);


#ifdef LATENCY_TRACK
void dump(vector<long long> latency)
{
    ofstream file;
    file.open("C:/latency.txt");
    
    for (int i = 0; i < latency.size(); i++)
        file << latency[i] << endl;

    file.close();
}
#endif


#ifdef TRACK_SAMPLE
void dump(vector<float> latency)
{
    ofstream file;
    file.open("C:/samples.txt");

    for (int i = 0; i < latency.size(); i++)
        file << latency[i] << endl;

    file.close();
}
#endif


void transfer_data(float* data)
{
    if (device != INVALID_HANDLE_VALUE)
    {
        DWORD bytes_return = 0;
        if (!DeviceIoControl(device, IOCTL_PUSH_DATA, data, HOP_LENGTH * sizeof(float), nullptr, 0, &bytes_return, nullptr))
        {
            ReportSvcStatus(SERVICE_RUNNING, ERROR_ABANDONED_WAIT_0, 0);
        }
        //*/
        /*const int chunk_size = 128;
        const int npackets      = HOP_LENGTH / chunk_size;
        for (int i = 0; i < npackets; i++)
            if (!DeviceIoControl(device, IOCTL_PUSH_DATA, data + i * chunk_size, chunk_size * sizeof(float), nullptr, 0, &bytes_return, nullptr))
            {
                ReportSvcStatus(SERVICE_RUNNING, ERROR_ALERTED, 0);
            }
        //*/
    }
}

void transfer_queue()
{
    float* signal = nullptr;

#ifdef LATENCY_TRACK
    vector<long long> latency;
#endif

    while (control)
    {
        float* data = (float*)in_queue.dequeue();
        if (data == nullptr)
        {
            SleepShort(1);
            continue;
        }

        buffer.insert(buffer.end(), data, data + HOP_LENGTH);
        if (buffer.size() < NSAMPLES)
        {
            SleepShort(1);
            continue;
        }
        auto start = chrono::steady_clock::now();

        pipeline->put(buffer.data());
        if (use_pipeline) signal = pipeline->infer();
        else
        {
            signal = new float[HOP_LENGTH];
            MA_COPY_MEMORY(signal, data, sizeof(float) * HOP_LENGTH);
        }

        if (signal != nullptr) out_queue.enqueue((void*)signal);

        auto end = chrono::steady_clock::now();

        buffer.erase(buffer.begin(), buffer.begin() + HOP_LENGTH);
        delete[] data;

#ifdef LATENCY_TRACK
        latency.push_back(chrono::duration_cast<chrono::nanoseconds>(end - start).count());
#endif
    }

#ifdef LATENCY_TRACK
    dump(latency);
#endif
}


void data_to_driver()
{
#ifdef TRACK_SAMPLE
    vector<float> output_buffer;
#endif

    while (control)
    {
        float* data = (float*)out_queue.dequeue();
        if (data == nullptr)
        {
            SleepShort(1);
            continue;
        }
        transfer_data(data);

#ifdef TRACK_SAMPLE
        output_buffer.insert(output_buffer.end(), data, data + HOP_LENGTH);
#endif
        delete[] data;
    }

#ifdef TRACK_SAMPLE
    dump(output_buffer);
#endif
}


void capture_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    float* cache = new float[frameCount];
    MA_COPY_MEMORY(cache, pInput, frameCount * sizeof(float));

    in_queue.enqueue((void*)cache);
}




//
// Purpose: 
//   Entry point for the process
//
// Parameters:
//   None
// 
// Return value:
//   None, defaults to 0 (zero)
//
int __cdecl _tmain(int argc, TCHAR* argv[])
{
    // If command-line parameter is "install", install the service. 
    // Otherwise, the service is probably being started by the SCM.

    /*device = CreateFile(L"\\\\.\\DataTransferLink", GENERIC_ALL, 0, 0, OPEN_EXISTING, FILE_ATTRIBUTE_SYSTEM, 0);

    DWORD bytes_return = 0;
    float data[] = { 1, 2, 3, 4 };
    float receive[] = { 0, 0, 0, 0, 0 };
    cout << DeviceIoControl(device, IOCTL_TEST, data, 4 * sizeof(float), receive, 4 * sizeof(float), &bytes_return, (LPOVERLAPPED)NULL) << endl;

    for (int i = 0; i < 4; i++)
        cout << receive[i] << " ";
    cout << endl;

    CloseHandle(device);*/
    //SvcInit(argc, argv);
    //return 0;

    if (lstrcmpi(argv[1], TEXT("install")) == 0)
    {
        SvcInstall();
        return 0;
    }

    WCHAR name[] = SVCNAME;
    // TO_DO: Add any additional services for the process to this table.
    SERVICE_TABLE_ENTRY DispatchTable[] =
    {
        { name, (LPSERVICE_MAIN_FUNCTION)SvcMain },
        { NULL, NULL }
    };

    // This call returns when the service has stopped. 
    // The process should simply terminate when the call returns.

    if (!StartServiceCtrlDispatcher(DispatchTable))
    {
        WCHAR ptext[] = TEXT("StartServiceCtrlDispatcher");
        SvcReportEvent(ptext);
    }
}

//
// Purpose: 
//   Installs a service in the SCM database
//
// Parameters:
//   None
// 
// Return value:
//   None
//
VOID SvcInstall()
{
    SC_HANDLE schSCManager;
    SC_HANDLE schService;
    TCHAR szPath[MAX_PATH];

    if (!GetModuleFileName(NULL, szPath, MAX_PATH))
    {
        printf("Cannot install service (%d)\n", GetLastError());
        return;
    }

    // Get a handle to the SCM database. 

    schSCManager = OpenSCManager(
        NULL,                    // local computer
        NULL,                    // ServicesActive database 
        SC_MANAGER_ALL_ACCESS);  // full access rights 

    if (NULL == schSCManager)
    {
        printf("OpenSCManager failed (%d)\n", GetLastError());
        return;
    }

    // Create the service

    schService = CreateService(
        schSCManager,              // SCM database 
        SVCNAME,                   // name of service 
        SVCNAME,                   // service name to display 
        SERVICE_ALL_ACCESS,        // desired access 
        SERVICE_WIN32_OWN_PROCESS, // service type 
        SERVICE_DEMAND_START,      // start type 
        SERVICE_ERROR_NORMAL,      // error control type 
        szPath,                    // path to service's binary 
        NULL,                      // no load ordering group 
        NULL,                      // no tag identifier 
        NULL,                      // no dependencies 
        NULL,                      // LocalSystem account 
        NULL);                     // no password 

    if (schService == NULL)
    {
        printf("CreateService failed (%d)\n", GetLastError());
        CloseServiceHandle(schSCManager);
        return;
    }
    else printf("Service installed successfully\n");

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
}

//
// Purpose: 
//   Entry point for the service
//
// Parameters:
//   dwArgc - Number of arguments in the lpszArgv array
//   lpszArgv - Array of strings. The first string is the name of
//     the service and subsequent strings are passed by the process
//     that called the StartService function to start the service.
// 
// Return value:
//   None.
//
VOID WINAPI SvcMain(DWORD dwArgc, LPTSTR* lpszArgv)
{
    // Register the handler function for the service

    gSvcStatusHandle = RegisterServiceCtrlHandler(
        SVCNAME,
        SvcCtrlHandler);

    if (!gSvcStatusHandle)
    {
        WCHAR wstr[] = TEXT("RegisterServiceCtrlHandler");
        SvcReportEvent(wstr);
        return;
    }

    // These SERVICE_STATUS members remain as set here

    gSvcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    gSvcStatus.dwServiceSpecificExitCode = 0;

    // Report initial status to the SCM

    ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    // Perform service-specific initialization and work.

    SvcInit(dwArgc, lpszArgv);
}

//
// Purpose: 
//   The service code
//
// Parameters:
//   dwArgc - Number of arguments in the lpszArgv array
//   lpszArgv - Array of strings. The first string is the name of
//     the service and subsequent strings are passed by the process
//     that called the StartService function to start the service.
// 
// Return value:
//   None
//
VOID SvcInit(DWORD dwArgc, LPTSTR* lpszArgv)
{
    // TO_DO: Declare and set any required variables.
    //   Be sure to periodically call ReportSvcStatus() with 
    //   SERVICE_START_PENDING. If initialization fails, call
    //   ReportSvcStatus with SERVICE_STOPPED.

    // Create an event. The control handler function, SvcCtrlHandler,
    // signals this event when it receives the stop control code.

    ghSvcStopEvent = CreateEvent(
        NULL,    // default security attributes
        TRUE,    // manual reset event
        FALSE,   // not signaled
        NULL);   // no name

    if (ghSvcStopEvent == NULL)
    {
        ReportSvcStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    ma_context context;
    ma_context_init(NULL, 0, NULL, &context);

    ma_device_config mic_config = ma_device_config_init(ma_device_type_capture);
    mic_config.sampleRate = SAMPLERATE;
    mic_config.periodSizeInFrames = HOP_LENGTH;
    mic_config.capture.channels = NCHANELS;
    mic_config.capture.format = ma_format_f32;
    mic_config.capture.pDeviceID = 0;
    mic_config.dataCallback = capture_callback;

    ma_device mic;
    ma_device_init(&context, &mic_config, &mic);

    FourierTransform f;
    Input i(f);
    Output o(f);
    //Inference_2Models model(MODEL1_PATH, MODEL2_PATH);
    Inference_Combined model(MODEL_PATH);
    pipeline = new Pipeline(&i, &o, &model);
    //*/

    device = CreateFile(L"\\\\.\\DataTransferLink", GENERIC_ALL, 0, 0, OPEN_EXISTING, FILE_ATTRIBUTE_SYSTEM, 0);

    if (device == INVALID_HANDLE_VALUE)
    {
        ReportSvcStatus(SERVICE_STOPPED, 1, 0);
        return;
    }

    thread tq(transfer_queue), td(data_to_driver);
    ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 0);

    ma_device_start(&mic);
    Sleep(10);

    ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);

    WaitForSingleObject(ghSvcStopEvent, INFINITE);

    ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);

    control = false;
    tq.join();
    td.join();

    ma_device_uninit(&mic);
    ma_context_uninit(&context);
    CloseHandle(device);

    ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

//
// Purpose: 
//   Sets the current service status and reports it to the SCM.
//
// Parameters:
//   dwCurrentState - The current state (see SERVICE_STATUS)
//   dwWin32ExitCode - The system error code
//   dwWaitHint - Estimated time for pending operation, 
//     in milliseconds
// 
// Return value:
//   None
//
VOID ReportSvcStatus(DWORD dwCurrentState,
    DWORD dwWin32ExitCode,
    DWORD dwWaitHint)
{
    static DWORD dwCheckPoint = 1;

    // Fill in the SERVICE_STATUS structure.

    gSvcStatus.dwCurrentState = dwCurrentState;
    gSvcStatus.dwWin32ExitCode = dwWin32ExitCode;
    gSvcStatus.dwWaitHint = dwWaitHint;

    if (dwCurrentState == SERVICE_START_PENDING)
        gSvcStatus.dwControlsAccepted = 0;
    else gSvcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;

    if ((dwCurrentState == SERVICE_RUNNING) ||
        (dwCurrentState == SERVICE_STOPPED))
        gSvcStatus.dwCheckPoint = 0;
    else gSvcStatus.dwCheckPoint = dwCheckPoint++;

    // Report the status of the service to the SCM.
    SetServiceStatus(gSvcStatusHandle, &gSvcStatus);
}

//
// Purpose: 
//   Called by SCM whenever a control code is sent to the service
//   using the ControlService function.
//
// Parameters:
//   dwCtrl - control code
// 
// Return value:
//   None
//
VOID WINAPI SvcCtrlHandler(DWORD dwCtrl)
{
    // Handle the requested control code. 

    switch (dwCtrl)
    {
    case SERVICE_CONTROL_STOP:
        ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);

        // Signal the service to stop.

        SetEvent(ghSvcStopEvent);
        ReportSvcStatus(gSvcStatus.dwCurrentState, NO_ERROR, 0);

        return;

    case SERVICE_CONTROL_SWITCH_NOFILTER:
        ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);
        use_pipeline = false;
        ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);
        break;

    case SERVICE_CONTROL_SWITCH_PIPELINE:
        ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);
        use_pipeline = true;
        ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);
        break;

    default:
        ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);

        // Signal the service to stop.

        SetEvent(ghSvcStopEvent);
        ReportSvcStatus(gSvcStatus.dwCurrentState, NO_ERROR, 0);

        return;
    }

}

//
// Purpose: 
//   Logs messages to the event log
//
// Parameters:
//   szFunction - name of function that failed
// 
// Return value:
//   None
//
// Remarks:
//   The service must have an entry in the Application event log.
//
VOID SvcReportEvent(LPTSTR szFunction)
{
    HANDLE hEventSource;
    LPCTSTR lpszStrings[2];
    TCHAR Buffer[80];

    hEventSource = RegisterEventSource(NULL, SVCNAME);

    if (NULL != hEventSource)
    {
        StringCchPrintf(Buffer, 80, TEXT("%s failed with %d"), szFunction, GetLastError());

        lpszStrings[0] = SVCNAME;
        lpszStrings[1] = Buffer;

        ReportEvent(hEventSource,        // event log handle
            EVENTLOG_ERROR_TYPE, // event type
            0,                   // event category
            SVC_ERROR,           // event identifier
            NULL,                // no security identifier
            2,                   // size of lpszStrings array
            0,                   // no binary data
            lpszStrings,         // array of strings
            NULL);               // no binary data

        DeregisterEventSource(hEventSource);
    }
}