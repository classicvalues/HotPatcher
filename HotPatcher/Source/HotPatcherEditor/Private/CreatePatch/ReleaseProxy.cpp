﻿#include "CreatePatch/ReleaseProxy.h"
#include "CreatePatch/ScopedSlowTaskContext.h"
#include "FHotPatcherVersion.h"
#include "FlibPatchParserHelper.h"
#include "HotPatcherLog.h"

// engine header
#include "FlibHotPatcherEditorHelper.h"
#include "Misc/DateTime.h"
#include "CoreGlobals.h"
#include "Logging/LogMacros.h"
#include "CreatePatch/HotPatcherContext.h"

#define LOCTEXT_NAMESPACE "UExportRelease"

namespace ReleaseWorker
{
	// inport from paklist
	bool ImportPakListWorker(FHotPatcherReleaseContext& Context);
	// scan new release 
	bool ExportNewReleaseWorker(FHotPatcherReleaseContext& Context);
	// save release asset info
	bool SaveReleaseVersionWorker(FHotPatcherReleaseContext& Context);
	bool SaveReleaseConfigWorker(FHotPatcherReleaseContext& Context);
	// backup Metadata
	bool BakcupMetadataWorker(FHotPatcherReleaseContext& Context);
	// backup config
	bool BakcupProjectConfigWorker(FHotPatcherReleaseContext& Context);
	// display release summary
	bool ReleaseSummaryWorker(FHotPatcherReleaseContext& Context);

};

bool UReleaseProxy::DoExport()
{
	// TimeRecorder TotalTimeTR(TEXT("Generate the release total time"));
	GetSettingObject()->Init();
	bool bRet = true;
	FHotPatcherReleaseContext ReleaseContext;
	ReleaseContext.ContextSetting = GetSettingObject();
	ReleaseContext.UnrealPakSlowTask = NewObject<UScopedSlowTaskContext>();
	ReleaseContext.UnrealPakSlowTask->AddToRoot();
	ReleaseContext.Init();
	TArray<TFunction<bool(FHotPatcherReleaseContext&)>> ReleaseWorker;
	
	ReleaseWorker.Emplace(&::ReleaseWorker::ImportPakListWorker);
	ReleaseWorker.Emplace(&::ReleaseWorker::ExportNewReleaseWorker);
	ReleaseWorker.Emplace(&::ReleaseWorker::SaveReleaseVersionWorker);
	ReleaseWorker.Emplace(&::ReleaseWorker::SaveReleaseConfigWorker);
	ReleaseWorker.Emplace(&::ReleaseWorker::BakcupMetadataWorker);
	ReleaseWorker.Emplace(&::ReleaseWorker::BakcupProjectConfigWorker);
	ReleaseWorker.Emplace(&::ReleaseWorker::ReleaseSummaryWorker);
	ReleaseContext.UnrealPakSlowTask->init((float)ReleaseWorker.Num());

	for(TFunction<bool(FHotPatcherReleaseContext&)> Worker:ReleaseWorker)
	{
		if(Worker(ReleaseContext))
		{
			continue;
		}
		else
		{
			bRet = false;
			break;
		}
	}
	ReleaseContext.UnrealPakSlowTask->Final();
	ReleaseContext.Shurdown();
	return bRet;
}

namespace ReleaseWorker
{
	bool ImportPakListWorker(FHotPatcherReleaseContext& Context)
	{
		FText DiaLogMsg = NSLOCTEXT("ImportPakListWorker", "ImportPakListWorker", "Import Pak List.");
		Context.UnrealPakSlowTask->EnterProgressFrame(1.0, DiaLogMsg);
		TimeRecorder TotalTimeTR(TEXT("Import Paklist"));
		if(Context.GetSettingObject()->ByPakList)
		{
			if(Context.GetSettingObject()->PlatformsPakListFiles.Num())
			{
				Context.GetSettingObject()->ImportPakLists();
			}
		}
		return true;
	}

	bool ExportNewReleaseWorker(FHotPatcherReleaseContext& Context)
	{
		TimeRecorder TotalTimeTR(TEXT("Export Release Version Info"));
		
		FText DiaLogMsg = FText::Format(NSLOCTEXT("AnalysisRelease", "AnalysisReleaseVersionInfo", "Analysis Release {0} Assets info."), FText::FromString(Context.GetSettingObject()->GetVersionId()));
		Context.UnrealPakSlowTask->EnterProgressFrame(1.0, DiaLogMsg);
		Context.NewReleaseVersion = UFlibPatchParserHelper::ExportReleaseVersionInfo(
			Context.GetSettingObject()->GetVersionId(),
			TEXT(""),
			FDateTime::UtcNow().ToString(),
			Context.GetSettingObject()->GetAssetIncludeFiltersPaths(),
			Context.GetSettingObject()->GetAssetIgnoreFiltersPaths(),
			Context.GetSettingObject()->GetAssetRegistryDependencyTypes(),
			Context.GetSettingObject()->GetSpecifyAssets(),
			Context.GetSettingObject()->GetAddExternAssetsToPlatform(),
			Context.GetSettingObject()->IsIncludeHasRefAssetsOnly(),
			Context.GetSettingObject()->IsAnalysisFilterDependencies()
		);
		FString NewReleaseVersionStr;
		THotPatcherTemplateHelper::TSerializeStructAsJsonString(Context.NewReleaseVersion,NewReleaseVersionStr);
		UE_LOG(LogHotPatcher,Display,TEXT("New Version Release Config:\n%s"),*NewReleaseVersionStr);
		return true;
	}

	// save release asset info
	bool SaveReleaseVersionWorker(FHotPatcherReleaseContext& Context)
	{
		TimeRecorder TR(TEXT("Save new release version info"));
		FText DiaLogMsg = FText::Format(NSLOCTEXT("ExportReleaseJson", "ExportReleaseVersionInfoJson", "Export Release {0} Assets info to file."), FText::FromString(Context.GetSettingObject()->GetVersionId()));
		Context.UnrealPakSlowTask->EnterProgressFrame(1.0, DiaLogMsg);
		FString SaveToJson;
		if (THotPatcherTemplateHelper::TSerializeStructAsJsonString(Context.NewReleaseVersion, SaveToJson))
		{

			FString SaveToFile = FPaths::Combine(
				FPaths::Combine(Context.GetSettingObject()->GetSaveAbsPath(), Context.GetSettingObject()->GetVersionId()),
				FString::Printf(TEXT("%s_Release.json"), *Context.GetSettingObject()->GetVersionId())
			);
			bool runState = UFlibAssetManageHelper::SaveStringToFile(SaveToFile, SaveToJson);
			if (runState)
			{
				auto Message = LOCTEXT("ExportReleaseSuccessNotification", "Succeed to export HotPatcher Release Version.");
				if(IsRunningCommandlet())
				{
					Context.OnShowMsg.Broadcast(Message.ToString());
				}
				else
				{
					UFlibHotPatcherEditorHelper::CreateSaveFileNotify(Message, SaveToFile);
				}
				
			}
			UE_LOG(LogHotPatcher, Log, TEXT("HotPatcher Export RELEASE is %s."), runState ? TEXT("Success") : TEXT("FAILD"));
		}
		return true;
	}
	
	bool SaveReleaseConfigWorker(FHotPatcherReleaseContext& Context)
	{
		TimeRecorder TR(TEXT("Save new release config"));
		FText DiaLogMsg = FText::Format(NSLOCTEXT("ExportReleaseConfig", "ExportReleaseConfigJson", "Export Release {0} Configuration to file."), FText::FromString(Context.GetSettingObject()->GetVersionId()));
		Context.UnrealPakSlowTask->EnterProgressFrame(1.0, DiaLogMsg);
		FString ConfigJson;
		if (THotPatcherTemplateHelper::TSerializeStructAsJsonString(*Context.GetSettingObject(),ConfigJson))
		{
			FString SaveToFile = FPaths::Combine(
				FPaths::Combine(Context.GetSettingObject()->GetSaveAbsPath(), Context.GetSettingObject()->GetVersionId()),
				FString::Printf(TEXT("%s_ReleaseConfig.json"), *Context.GetSettingObject()->GetVersionId())
			);
			bool runState = UFlibAssetManageHelper::SaveStringToFile(SaveToFile, ConfigJson);
			if (runState)
			{
				auto Message = LOCTEXT("ExportReleaseConfigSuccessNotification", "Succeed to export HotPatcher Release Config.");
				if(::IsRunningCommandlet())
				{
					Context.OnShowMsg.Broadcast(Message.ToString());
				}
				else
				{
					UFlibHotPatcherEditorHelper::CreateSaveFileNotify(Message, SaveToFile);
				}
			}
			UE_LOG(LogHotPatcher, Log, TEXT("HotPatcher Export RELEASE CONFIG is %s."), runState ? TEXT("Success") : TEXT("FAILD"));
		}
		return true;
	}

	// backup Metadata
	bool BakcupMetadataWorker(FHotPatcherReleaseContext& Context)
	{
		TimeRecorder TR(TEXT("Backup Metadata"));
		FText DiaLogMsg = FText::Format(NSLOCTEXT("BackupMetadata", "BackupMetadata", "Backup Release {0} Metadatas."), FText::FromString(Context.GetSettingObject()->GetVersionId()));
		Context.UnrealPakSlowTask->EnterProgressFrame(1.0, DiaLogMsg);
		if(Context.GetSettingObject()->IsBackupMetadata())
		{
			UFlibHotPatcherEditorHelper::BackupMetadataDir(
				FPaths::ProjectDir(),
				FApp::GetProjectName(),
				Context.GetSettingObject()->GetBackupMetadataPlatforms(),
			FPaths::Combine(Context.GetSettingObject()->GetSaveAbsPath(),
				Context.GetSettingObject()->GetVersionId())
			);
		}
		return true;
	}
	// backup project config
	bool BakcupProjectConfigWorker(FHotPatcherReleaseContext& Context)
	{
		TimeRecorder TR(TEXT("Backup Config"));
		FText DiaLogMsg = FText::Format(NSLOCTEXT("BackupProjectConfig", "BackupProjectConfig", "Backup Release {0} Configs."), FText::FromString(Context.GetSettingObject()->GetVersionId()));
		Context.UnrealPakSlowTask->EnterProgressFrame(1.0, DiaLogMsg);
		if(Context.GetSettingObject()->IsBackupProjectConfig())
		{
			UFlibHotPatcherEditorHelper::BackupProjectConfigDir(
				FPaths::ProjectDir(),
				FPaths::Combine(Context.GetSettingObject()->GetSaveAbsPath(),Context.GetSettingObject()->GetVersionId())
			);
		}
		return true;
	}

	bool ReleaseSummaryWorker(FHotPatcherReleaseContext& Context)
	{
		FText DiaLogMsg = NSLOCTEXT("ReleaseSummaryWorker", "ReleaseSummaryWorker", "Release Summary.");
		Context.UnrealPakSlowTask->EnterProgressFrame(1.0, DiaLogMsg);
		TimeRecorder TR(TEXT("Generate Release Summary"));
		Context.OnShowMsg.Broadcast(UFlibHotPatcherEditorHelper::ReleaseSummary(Context.NewReleaseVersion));
		return true;
	}
	
};
#undef LOCTEXT_NAMESPACE
