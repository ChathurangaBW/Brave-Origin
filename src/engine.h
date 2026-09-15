#pragma once
#include <windows.h>
#include <string>

enum class EngineStatus { NotInstalled, NeedsRepair, Ready, Error };
struct EngineChannelSnapshot {
    std::wstring channel, profilePath, localStatePath, detail;
    EngineStatus status = EngineStatus::NotInstalled;
    bool exists = false;
    unsigned long long size = 0;
    FILETIME lastWrite = {};
};

int EngineRunCli();
EngineChannelSnapshot EngineScanChannel(const std::wstring& channel);
int EngineApplyChannel(const std::wstring& channel, std::wstring* backupPath);
int EngineRestoreBackup(const std::wstring& backupPath, std::wstring* currentBackupPath = nullptr);
