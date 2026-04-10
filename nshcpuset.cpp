#define _CRT_SECURE_NO_WARNINGS
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <wincrypt.h>

#pragma comment(lib, "Version.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Crypt32.lib")

#define MAX_CPUSETS 1024
#define MAX_MATCHES 512

typedef struct
{
    ULONG id;
    ULONG logicalIndex;
    ULONG efficiencyClass;
} CPU_ENTRY;

CPU_ENTRY cpuList[MAX_CPUSETS];
ULONG cpuCount = 0;

/* -------------------------------------------------- */
/* CPU SET DETECTION                                  */
/* -------------------------------------------------- */
void CollectCpuSets()
{
    ULONG len = 0;
    GetSystemCpuSetInformation(NULL, 0, &len, NULL, 0);

    PSYSTEM_CPU_SET_INFORMATION buffer = (PSYSTEM_CPU_SET_INFORMATION) malloc(len);

    if (!GetSystemCpuSetInformation(buffer, len, &len, NULL, 0))
    {
        printf("GetSystemCpuSetInformation failed\n");
        exit(1);
    }

    BYTE *ptr = (BYTE *)buffer;
    BYTE *end = ptr + len;

    while (ptr < end)
    {
        PSYSTEM_CPU_SET_INFORMATION info = (PSYSTEM_CPU_SET_INFORMATION)ptr;

        if (info->Type == CpuSetInformation)
        {
            cpuList[cpuCount].id = info->CpuSet.Id;
            cpuList[cpuCount].logicalIndex = info->CpuSet.LogicalProcessorIndex;
            cpuList[cpuCount].efficiencyClass = info->CpuSet.EfficiencyClass;
            cpuCount++;
        }

        ptr += info->Size;
    }

    free(buffer);
}

ULONG FindMaxEfficiency()
{
    ULONG max = 0;

    for (ULONG i = 0; i < cpuCount; i++)
        if (cpuList[i].efficiencyClass > max)
            max = cpuList[i].efficiencyClass;

    return max;
}

void PrintCpuInfo(ULONG maxClass)
{
    size_t Count_ECore = 0;
    size_t Count_PCore = 0;

    printf("\nCPU layout:\n");
    printf("----------------------------------------\n");

    for (ULONG i = 0; i < cpuCount; i++)
    {
        printf("CPU %2lu -> EfficiencyClass=%lu %s\n",
               cpuList[i].logicalIndex,
               cpuList[i].efficiencyClass,
               (cpuList[i].efficiencyClass == maxClass) ? "(P-core)" : "(E-core)");

        if (cpuList[i].efficiencyClass == maxClass)
            Count_PCore++;
        else
            Count_ECore++;
    }

    printf ("\n");
    printf ("P-Cores : %zu\n", Count_PCore);
    printf ("E-Cores : %zu\n", Count_ECore);
}

/* -------------------------------------------------- */
/* VERSION INFO (Company)                             */
/* -------------------------------------------------- */
int GetCompanyName(const char *path, char *out, size_t outSize)
{
    DWORD dummy;
    DWORD size = GetFileVersionInfoSizeA(path, &dummy);

    if (size == 0)
        return 0;

    BYTE *buffer = (BYTE *) malloc(size);

    if (!GetFileVersionInfoA(path, 0, size, buffer))
    {
        free(buffer);
        return 0;
    }

    struct LANGANDCODEPAGE
    {
        WORD wLanguage;
        WORD wCodePage;
    } *lpTranslate;

    UINT len;

    if (!VerQueryValueA(buffer,
                        "\\VarFileInfo\\Translation",
                        (LPVOID *)&lpTranslate,
                        &len))
    {
        free(buffer);
        return 0;
    }

    char subBlock[256];
    sprintf(subBlock,
            "\\StringFileInfo\\%04x%04x\\CompanyName",
            lpTranslate[0].wLanguage,
            lpTranslate[0].wCodePage);

    char *company = NULL;

    if (VerQueryValueA(buffer, subBlock, (LPVOID *)&company, &len))
    {
        strncpy(out, company, outSize - 1);
        out[outSize - 1] = 0;

        free(buffer);
        return 1;
    }

    free(buffer);
    return 0;
}

/* -------------------------------------------------- */
/* SIGNER                                             */
/* -------------------------------------------------- */
int GetSigner(const wchar_t *path, char *out, size_t outSize)
{
    HCERTSTORE hStore = NULL;
    HCRYPTMSG hMsg = NULL;
    DWORD encoding, contentType, formatType;

    if (!CryptQueryObject(
            CERT_QUERY_OBJECT_FILE,
            path,
            CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
            CERT_QUERY_FORMAT_FLAG_BINARY,
            0,
            &encoding,
            &contentType,
            &formatType,
            &hStore,
            &hMsg,
            NULL))
    {
        return 0;
    }

    DWORD signerInfoSize = 0;
    CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &signerInfoSize);

    PCMSG_SIGNER_INFO signerInfo = (PCMSG_SIGNER_INFO) malloc(signerInfoSize);

    if (!CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, signerInfo, &signerInfoSize))
    {
        free(signerInfo);
        return 0;
    }

    CERT_INFO certInfo;
    certInfo.Issuer = signerInfo->Issuer;
    certInfo.SerialNumber = signerInfo->SerialNumber;

    PCCERT_CONTEXT certContext = CertFindCertificateInStore(
        hStore,
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
        0,
        CERT_FIND_SUBJECT_CERT,
        &certInfo,
        NULL);

    if (!certContext)
    {
        free(signerInfo);
        return 0;
    }

    CertGetNameStringA(certContext,
                       CERT_NAME_SIMPLE_DISPLAY_TYPE,
                       0,
                       NULL,
                       out,
                       (DWORD)outSize);

    CertFreeCertificateContext(certContext);
    free(signerInfo);
    CertCloseStore(hStore, 0);
    CryptMsgClose(hMsg);

    return 1;
}

/* -------------------------------------------------- */
/* APPLY CPU SETS                                     */
/* -------------------------------------------------- */

void SetProcessPCores(DWORD pid, ULONG maxClass)
{
    HANDLE hProc = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

    if (!hProc)
    {
        printf("OpenProcess failed PID=%lu (%lu)\n", pid, GetLastError());
        return;
    }

    // --- Set priority ---
    if (!SetPriorityClass(hProc, ABOVE_NORMAL_PRIORITY_CLASS))
    {
        printf("SetPriorityClass failed PID=%lu (%lu)\n", pid, GetLastError());
    }
    else
    {
        printf("Adjust priority for PID=%lu\n", pid);
    }

    // --- Set CPU Sets ---
    ULONG ids[MAX_CPUSETS];
    ULONG count = 0;

    for (ULONG i = 0; i < cpuCount; i++)
        if (cpuList[i].efficiencyClass == maxClass)
            ids[count++] = cpuList[i].id;

    if (!SetProcessDefaultCpuSets(hProc, ids, count))
    {
        printf("SetProcessDefaultCpuSets failed PID=%lu (%lu)\n", pid, GetLastError());
    }
    else
    {
        printf("Applied P-core preference to PID=%lu (%lu cores)\n", pid, count);
    }

    CloseHandle(hProc);
}

/* -------------------------------------------------- */
/* MODE: SCAN                                         */
/* -------------------------------------------------- */
void ScanHCL()
{
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (Process32First(snap, &pe))
    {
        do
        {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!hProc)
                continue;

            char path[MAX_PATH];
            DWORD size = MAX_PATH;

            if (QueryFullProcessImageNameA(hProc, 0, path, &size))
            {
                char company[256] = {0};
                char signer[256] = {0};

                int match = 0;

                if (GetCompanyName(path, company, sizeof(company)))
                    if (StrStrIA(company, "HCL") || StrStrIA(company, "IBM"))
                        match = 1;

                wchar_t wpath[MAX_PATH];
                MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);

                if (GetSigner(wpath, signer, sizeof(signer)))
                    if (StrStrIA(signer, "HCL") || StrStrIA(signer, "IBM"))
                        match = 1;

                if (match)
                {
                    printf("Matched: %-20s PID=%lu\n", pe.szExeFile, pe.th32ProcessID);
                    printf("  Company: %s\n", company);
                    printf("  Signer : %s\n\n", signer);
                }
            }

            CloseHandle(hProc);

        } while (Process32Next(snap, &pe));
    }

    CloseHandle(snap);
}

/* -------------------------------------------------- */
/* MODE: SET                                          */
/* -------------------------------------------------- */
void setByName(char *filter, ULONG maxClass)
{
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (Process32First(snap, &pe))
    {
        do
        {
            if (StrStrIA(pe.szExeFile, filter))
            {
                printf("Applying to %s PID=%lu\n", pe.szExeFile, pe.th32ProcessID);
                SetProcessPCores(pe.th32ProcessID, maxClass);
            }

        } while (Process32Next(snap, &pe));
    }

    CloseHandle(snap);
}

/* -------------------------------------------------- */
/* MAIN                                               */
/* -------------------------------------------------- */
int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("Usage:\n");
        printf("  -scan              List HCL processes\n");
        printf("  -set <name>        Apply to process name or substring\n");
        return 0;
    }

    CollectCpuSets();
    ULONG maxClass = FindMaxEfficiency();
    PrintCpuInfo(maxClass);

    printf ("\n");

    if (_stricmp(argv[1], "-scan") == 0)
    {
        ScanHCL();
        printf ("\n");
    }
    else if (_stricmp(argv[1], "-set") == 0 && argc > 2)
    {
        char *token = strtok(argv[2], ",");

        while (token)
        {
            setByName(token, maxClass);
            token = strtok(NULL, ",");
            printf ("\n");
        }
    }

    return 0;
}