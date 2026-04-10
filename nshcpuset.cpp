
#define VERSION "1.0.0"

#define _CRT_SECURE_NO_WARNINGS // OK here for a test tool
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

CPU_ENTRY gCpuList[MAX_CPUSETS] = {0};
ULONG gCpuCount = 0;

/* -------------------------------------------------- */
/* CPU SET DETECTION                                  */
/* -------------------------------------------------- */
void CollectCpuSets()
{
    ULONG len = 0;
    GetSystemCpuSetInformation(NULL, 0, &len, NULL, 0);

    PSYSTEM_CPU_SET_INFORMATION pBuffer = (PSYSTEM_CPU_SET_INFORMATION) malloc(len);

    if (!GetSystemCpuSetInformation(pBuffer, len, &len, NULL, 0))
    {
        printf("GetSystemCpuSetInformation failed\n");
        exit(1);
    }

    BYTE *ptr = (BYTE *)pBuffer;
    BYTE *end = ptr + len;

    while (ptr < end)
    {
        PSYSTEM_CPU_SET_INFORMATION info = (PSYSTEM_CPU_SET_INFORMATION)ptr;

        if (info->Type == CpuSetInformation)
        {
            gCpuList[gCpuCount].id = info->CpuSet.Id;
            gCpuList[gCpuCount].logicalIndex = info->CpuSet.LogicalProcessorIndex;
            gCpuList[gCpuCount].efficiencyClass = info->CpuSet.EfficiencyClass;
            gCpuCount++;
        }

        ptr += info->Size;
    }

    free(pBuffer);
}

ULONG FindMaxEfficiency()
{
    ULONG max = 0;

    for (ULONG i = 0; i < gCpuCount; i++)
        if (gCpuList[i].efficiencyClass > max)
            max = gCpuList[i].efficiencyClass;

    return max;
}

void PrintCpuInfo(ULONG MaxClass)
{
    size_t Count_ECore = 0;
    size_t Count_PCore = 0;

    printf("\nCPU layout:\n");
    printf("----------------------------------------\n");

    for (ULONG i = 0; i < gCpuCount; i++)
    {
        printf("CPU %2lu -> EfficiencyClass=%lu %s\n",
               gCpuList[i].logicalIndex,
               gCpuList[i].efficiencyClass,
               (gCpuList[i].efficiencyClass == MaxClass) ? "(P-core)" : "(E-core)");

        if (gCpuList[i].efficiencyClass == MaxClass)
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
int GetCompanyName(const char *pszPath, char *out, size_t outSize)
{
    int   ret   = 0;
    DWORD dwDummy = 0;
    DWORD dwSize = GetFileVersionInfoSizeA(pszPath, &dwDummy);
    UINT len = 0;
    char szSubBlock[2048] = {0};
    char *pCompany = NULL;

    BYTE *pBuffer = NULL;

    struct LANGANDCODEPAGE
    {
        WORD wLanguage;
        WORD wCodePage;
    } *lpTranslate;

    if (dwSize == 0)
        return 0;

    pBuffer = (BYTE *) malloc(dwSize);

    if (!GetFileVersionInfoA(pszPath, 0, dwSize, pBuffer))
    {
        goto Done;
    }

    if (!VerQueryValueA(pBuffer,
                        "\\VarFileInfo\\Translation",
                        (LPVOID *)&lpTranslate,
                        &len))
    {
        goto Done;
    }

    sprintf(szSubBlock,
            "\\StringFileInfo\\%04x%04x\\CompanyName",
            lpTranslate[0].wLanguage,
            lpTranslate[0].wCodePage);

    if (VerQueryValueA(pBuffer, szSubBlock, (LPVOID *)&pCompany, &len))
    {
        strncpy(out, pCompany, outSize - 1);
        out[outSize - 1] = 0;

        ret = 1;
        goto Done;
    }

Done:

    if (pBuffer)
    {
        free(pBuffer);
        pBuffer = NULL;
    }

    return 0;
}

/* -------------------------------------------------- */
/* SIGNER                                             */
/* -------------------------------------------------- */
int GetSigner(const wchar_t *pswzPath, char *out, size_t outSize)
{
    HCERTSTORE hStore = NULL;
    HCRYPTMSG hMsg    = NULL;
    DWORD encoding, contentType, formatType;

    if (!CryptQueryObject(
            CERT_QUERY_OBJECT_FILE,
            pswzPath,
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


const char* PriorityToString(DWORD p)
{
    switch (p)
    {
        case IDLE_PRIORITY_CLASS: return "IDLE";
        case BELOW_NORMAL_PRIORITY_CLASS: return "BELOW_NORMAL";
        case NORMAL_PRIORITY_CLASS: return "NORMAL";
        case ABOVE_NORMAL_PRIORITY_CLASS: return "ABOVE_NORMAL";
        case HIGH_PRIORITY_CLASS: return "HIGH";
        case REALTIME_PRIORITY_CLASS: return "REALTIME";
        default: return "UNKNOWN";
    }
}


/* -------------------------------------------------- */
/* APPLY CPU SETS                                     */
/* -------------------------------------------------- */

int cmp_ulong(const void *a, const void *b)
{
    ULONG x = *(const ULONG *)a;
    ULONG y = *(const ULONG *)b;

    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

/* -------------------------------------------------- */
/* APPLY CPU SETS                                     */
/* -------------------------------------------------- */

void SetProcessPCores(DWORD pid, ULONG MaxClass, DWORD NewPriorityClass)
{
    ULONG SetIDs[MAX_CPUSETS] = {0};
    ULONG GetIDs[MAX_CPUSETS] = {0};

    ULONG NewCount = 0;
    ULONG CurrentCount = 0;
    ULONG Needed = 0;

    DWORD dwCurrentPriorityClass = 0;
    BOOL  fNeedsUpdate = FALSE;

    HANDLE hProc = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

    if (!hProc)
    {
        printf("OpenProcess failed PID=%lu (%lu)\n", pid, GetLastError());
        return;
    }

    /* ---------------- Priority ---------------- */

    dwCurrentPriorityClass = GetPriorityClass(hProc);

    if (dwCurrentPriorityClass == 0)
    {
        printf("GetPriorityClass failed (%lu)\n", GetLastError());
    }
    else if (NewPriorityClass == dwCurrentPriorityClass)
    {
        printf("Priority for PID=%lu is already %s\n",
               pid, PriorityToString(dwCurrentPriorityClass));
    }
    else
    {
        if (!SetPriorityClass(hProc, NewPriorityClass))
        {
            printf("SetPriorityClass failed PID=%lu (%lu)\n", pid, GetLastError());
        }
        else
        {
            printf("Adjust priority for PID=%lu (%s -> %s)\n",
                   pid,
                   PriorityToString(dwCurrentPriorityClass),
                   PriorityToString(NewPriorityClass));
        }
    }

    /* ---------------- Build desired CPU set ---------------- */

    for (ULONG i = 0; i < gCpuCount; i++)
    {
        if (gCpuList[i].efficiencyClass == MaxClass)
        {
            SetIDs[NewCount++] = gCpuList[i].id;
        }
    }

    /* ---------------- Query current CPU sets ---------------- */

    if (!GetProcessDefaultCpuSets(hProc, GetIDs, MAX_CPUSETS, &CurrentCount))
    {
        printf("GetProcessDefaultCpuSets failed (%lu)\n", GetLastError());
        goto Done;
    }

    /* ---------------- Compare ---------------- */

    if (CurrentCount == 0)
    {
        printf("Process PID=%lu currently unrestricted (all CPUs allowed)\n", pid);
        fNeedsUpdate = TRUE;
    }
    else if (CurrentCount != NewCount)
    {
        fNeedsUpdate = TRUE;
    }
    else
    {
        /* sort both arrays before comparing */
        qsort(SetIDs, NewCount, sizeof(ULONG), cmp_ulong);
        qsort(GetIDs, CurrentCount, sizeof(ULONG), cmp_ulong);

        for (ULONG i = 0; i < NewCount; i++)
        {
            if (SetIDs[i] != GetIDs[i])
            {
                fNeedsUpdate = TRUE;
                break;
            }
        }
    }

    /* ---------------- Apply if needed ---------------- */

    if (fNeedsUpdate)
    {
        if (!SetProcessDefaultCpuSets(hProc, SetIDs, NewCount))
        {
            printf("SetProcessDefaultCpuSets failed PID=%lu (%lu)\n", pid, GetLastError());
        }
        else
        {
            printf("Applied P-core preference to PID=%lu (%lu cores)\n", pid, NewCount);
        }
    }
    else
    {
        printf("P-core preference for PID=%lu already set (%lu cores)\n", pid, NewCount);
    }

Done:

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

            char pszPath[MAX_PATH+1] = {0};
            DWORD size = MAX_PATH;

            if (QueryFullProcessImageNameA(hProc, 0, pszPath, &size))
            {
                char pCompany[256] = {0};
                char signer[256]   = {0};

                int match = 0;

                if (GetCompanyName(pszPath, pCompany, sizeof(pCompany)))
                    if (StrStrIA(pCompany, "HCL") || StrStrIA(pCompany, "IBM"))
                        match = 1;

                wchar_t wpath[MAX_PATH];
                MultiByteToWideChar(CP_UTF8, 0, pszPath, -1, wpath, MAX_PATH);

                if (GetSigner(wpath, signer, sizeof(signer)))
                    if (StrStrIA(signer, "HCL") || StrStrIA(signer, "IBM"))
                        match = 1;

                if (match)
                {
                    printf("Matched: %-20s PID=%lu\n", pe.szExeFile, pe.th32ProcessID);
                    printf("  Company: %s\n", pCompany);
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
void SetByName(char *pszFilter, ULONG MaxClass, DWORD NewPriorityClass)
{
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);

    if (NULL == pszFilter)
        return;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (Process32First(snap, &pe))
    {
        do
        {
            if (StrStrIA(pe.szExeFile, pszFilter))
            {
                printf("\n--- %s PID=%lu ---\n", pe.szExeFile, pe.th32ProcessID);
                SetProcessPCores(pe.th32ProcessID, MaxClass, NewPriorityClass);
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
    DWORD NewPriorityClass = ABOVE_NORMAL_PRIORITY_CLASS;

    printf ("\nnshCpuSet %s\n\n", VERSION);

    if (argc < 2)
    {
        printf("Usage:\n");
        printf("  -scan        List HCL processes\n");
        printf("  -set <name>  Apply to process name or substring\n");
        return 0;
    }

    CollectCpuSets();
    ULONG MaxClass = FindMaxEfficiency();
    PrintCpuInfo(MaxClass);

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
            SetByName(token, MaxClass, NewPriorityClass);
            token = strtok(NULL, ",");
            printf ("\n");
        }
    }

    return 0;
}

