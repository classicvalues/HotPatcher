// Copyright 2019 Lipeng Zha, Inc. All Rights Reserved.

#include "HotPatcherEditor.h"
#include "HotPatcherStyle.h"
#include "HotPatcherCommands.h"
#include "SHotPatcher.h"
#include "HotPatcherSettings.h"
#include "CookManager.h"

// ENGINE HEADER

#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Misc/MessageDialog.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "DesktopPlatformModule.h"
#include "FlibHotPatcherEditorHelper.h"
#include "HotPatcherLog.h"
#include "ISettingsModule.h"
#include "LevelEditor.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet/KismetTextLibrary.h"
#include "PakFileUtilities.h"
#include "CreatePatch/PatcherProxy.h"

#if ENGINE_MAJOR_VERSION < 5
#include "Widgets/Docking/SDockableTab.h"
#endif

#if WITH_EDITOR_SECTION
#include "ToolMenus.h"
#include "ToolMenuDelegates.h"
#include "ContentBrowserMenuContexts.h"
#include "CreatePatch/AssetActions/AssetTypeActions_PrimaryAssetLabel.h"
#endif

FExportPatchSettings* GPatchSettings = nullptr;
FExportReleaseSettings* GReleaseSettings = nullptr;
bool GCookLog = true;

static const FName HotPatcherTabName("HotPatcher");

#define LOCTEXT_NAMESPACE "FHotPatcherEditorModule"

void ReceiveOutputMsg(FProcWorkerThread* Worker,const FString& InMsg)
{
	FString FindItem(TEXT("Display:"));
	int32 Index= InMsg.Len() - InMsg.Find(FindItem)- FindItem.Len();
	if (InMsg.Contains(TEXT("Error:")))
	{
		UE_LOG(LogHotPatcher, Error, TEXT("%s"), *InMsg);
	}
	else if (InMsg.Contains(TEXT("Warning:")))
	{
		UE_LOG(LogHotPatcher, Warning, TEXT("%s"), *InMsg);
	}
	else
	{
		UE_LOG(LogHotPatcher, Display, TEXT("%s"), *InMsg.Right(Index));
	}
}

void MakeProjectSettingsForHotPatcher()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->RegisterSettings("Project", "Plugins", "Hot Patcher",
            LOCTEXT("HotPatcherSettingsName", "Hot Patcher"),
            LOCTEXT("HotPatcherSettingsDescroption", "Configure the HotPatcher plugin"),
            GetMutableDefault<UHotPatcherSettings>());
	}
}

FHotPatcherEditorModule& FHotPatcherEditorModule::Get()
{
	FHotPatcherEditorModule& Module = FModuleManager::GetModuleChecked<FHotPatcherEditorModule>("HotPatcherEditor");
	return Module;
}

void FHotPatcherEditorModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module

	FHotPatcherStyle::Initialize();
	FHotPatcherStyle::ReloadTextures();
	FHotPatcherCommands::Register();

	FParse::Bool(FCommandLine::Get(),TEXT("-cooklog"),GCookLog);
	UE_LOG(LogHotPatcher,Log,TEXT("GCookLog is %s!!!"),GCookLog ? TEXT("TRUE"): TEXT("FALSE"));
	
	if(::IsRunningCommandlet())
		return;
	
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	FCoreUObjectDelegates::OnObjectSaved.AddRaw(this,&FHotPatcherEditorModule::OnObjectSaved);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	FCookManager::Get().Init();
	MakeProjectSettingsForHotPatcher();

	MissionNotifyProay = NewObject<UMissionNotificationProxy>();
	MissionNotifyProay->AddToRoot();
	
	PluginCommands = MakeShareable(new FUICommandList);
	PluginCommands->MapAction(
		FHotPatcherCommands::Get().PluginAction,
		FExecuteAction::CreateRaw(this, &FHotPatcherEditorModule::PluginButtonClicked),
		FCanExecuteAction());
	
	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	{
		TSharedPtr<FExtender> MenuExtender = MakeShareable(new FExtender());
		MenuExtender->AddMenuExtension("WindowLayout", EExtensionHook::After, PluginCommands, FMenuExtensionDelegate::CreateRaw(this, &FHotPatcherEditorModule::AddMenuExtension));

		LevelEditorModule.GetMenuExtensibilityManager()->AddExtender(MenuExtender);

		// settings
	 	TSharedPtr<FExtender> ToolbarExtender = MakeShareable(new FExtender);
	 	ToolbarExtender->AddToolBarExtension("Settings", EExtensionHook::After, PluginCommands, FToolBarExtensionDelegate::CreateRaw(this, &FHotPatcherEditorModule::AddToolbarExtension));

	 	LevelEditorModule.GetToolBarExtensibilityManager()->AddExtender(ToolbarExtender);
	}

	TSharedRef<FUICommandList> CommandList = LevelEditorModule.GetGlobalLevelEditorActions();
	CommandList->UnmapAction(FHotPatcherCommands::Get().CookSelectedAction);
	CommandList->UnmapAction(FHotPatcherCommands::Get().CookAndPakSelectedAction);
	CommandList->UnmapAction(FHotPatcherCommands::Get().AddToPakSettingsAction);

#if WITH_EDITOR_SECTION
	ExtendContentBrowserAssetSelectionMenu();
	ExtendContentBrowserPathSelectionMenu();
	
	CreateRootMenu();

	FAssetToolsModule::GetModule().Get().RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_PrimaryAssetLabel));
#endif
}



void FHotPatcherEditorModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
	FCookManager::Get().Shutdown();
	if(DockTab.IsValid())
	{
		DockTab->RequestCloseTab();
	}
	
	FHotPatcherCommands::Unregister();
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(HotPatcherTabName);
	FHotPatcherStyle::Shutdown();
}

void FHotPatcherEditorModule::OpenDockTab()
{
	if(!DockTab.IsValid() || !GPatchSettings)
	{
		PluginButtonClicked();
	}
}

void FHotPatcherEditorModule::PluginButtonClicked()
{
	if(!DockTab.IsValid())
	{
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(HotPatcherTabName, FOnSpawnTab::CreateRaw(this, &FHotPatcherEditorModule::OnSpawnPluginTab))
	    .SetDisplayName(LOCTEXT("FHotPatcherTabTitle", "HotPatcher"))
	    .SetMenuType(ETabSpawnerMenuType::Hidden);
	}
	FGlobalTabmanager::Get()->InvokeTab(HotPatcherTabName);
}

void FHotPatcherEditorModule::OnTabClosed(TSharedRef<SDockTab> InTab)
{
	DockTab.Reset();
}

void FHotPatcherEditorModule::AddMenuExtension(FMenuBuilder& Builder)
{
	Builder.AddMenuEntry(FHotPatcherCommands::Get().PluginAction);
}


void FHotPatcherEditorModule::AddToolbarExtension(FToolBarBuilder& Builder)
{
	Builder.AddToolBarButton(FHotPatcherCommands::Get().PluginAction);
}



TArray<FAssetData> FHotPatcherEditorModule::GetSelectedAssetsInBrowserContent()
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	TArray<FAssetData> AssetsData;
	ContentBrowserModule.Get().GetSelectedAssets(AssetsData);
	return AssetsData;
}

TArray<FString> FHotPatcherEditorModule::GetSelectedFolderInBrowserContent()
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	TArray<FString> Folders;
	ContentBrowserModule.Get().GetSelectedFolders(Folders);
	return Folders;
}


TSharedRef<class SDockTab> FHotPatcherEditorModule::OnSpawnPluginTab(const class FSpawnTabArgs& InSpawnTabArgs)
{
	return SAssignNew(DockTab,SDockTab)
		.TabRole(ETabRole::NomadTab)
		.Label(LOCTEXT("HotPatcherTab", "Hot Patcher"))
		.ToolTipText(LOCTEXT("HotPatcherTabTextToolTip", "Hot Patcher"))
		.OnTabClosed(SDockTab::FOnTabClosedCallback::CreateRaw(this,&FHotPatcherEditorModule::OnTabClosed))
		.Clipping(EWidgetClipping::ClipToBounds)
		[
			SNew(SHotPatcher)
		];
}


#if WITH_EDITOR_SECTION 

void FHotPatcherEditorModule::CreateRootMenu()
{
	UToolMenu* RootMenu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AddNewContextMenu");
	FToolMenuSection& RootSection = RootMenu->AddSection("HotPatcherUtilities", LOCTEXT("HotPatcherUtilities", "HotPatcherUtilities"));;
	RootSection.AddSubMenu(
		"PatcherPresetsActionsSubMenu",
		LOCTEXT("PatcherPresetsActionsSubMenuLabel", "HotPatcher Preset Actions"),
		LOCTEXT("PatcherPresetsActionsSubMenuToolTip", "HotPatcher Preset Actions"),
		FNewToolMenuDelegate::CreateRaw(this, &FHotPatcherEditorModule::MakeHotPatcherPresetsActionsSubMenu),
		FUIAction(
			FExecuteAction()
			),
		EUserInterfaceActionType::Button,
		false, 
		FSlateIcon(FEditorStyle::GetStyleSetName(), "ContentBrowser.SaveAllCurrentFolder")
		);
	
	RootSection.AddSubMenu(
		"PakExternalFilesActionsSubMenu",
		LOCTEXT("PakExternalFilesActionsSubMenuLabel", "Pak External Actions"),
		LOCTEXT("PakExternalFilesActionsSubMenuToolTip", "Pak External Actions"),
		FNewToolMenuDelegate::CreateRaw(this, &FHotPatcherEditorModule::MakePakExternalActionsSubMenu),
		FUIAction(
			FExecuteAction()
			),
		EUserInterfaceActionType::Button,
		false, 
		FSlateIcon(FEditorStyle::GetStyleSetName(), "ContentBrowser.SaveAllCurrentFolder")
		);
}

void FHotPatcherEditorModule::CreateAssetContextMenu(FToolMenuSection& InSection)
{
	InSection.AddSubMenu(
		"CookActionsSubMenu",
		LOCTEXT("CookActionsSubMenuLabel", "Cook Actions"),
		LOCTEXT("CookActionsSubMenuToolTip", "Cook actions"),
		FNewToolMenuDelegate::CreateRaw(this, &FHotPatcherEditorModule::MakeCookActionsSubMenu),
		FUIAction(
			FExecuteAction()
			),
		EUserInterfaceActionType::Button,
		false, 
		FSlateIcon(FEditorStyle::GetStyleSetName(), "ContentBrowser.AssetActions")
		);
	InSection.AddSubMenu(
		"CookAndPakActionsSubMenu",
		LOCTEXT("CookAndPakActionsSubMenuLabel", "Cook And Pak Actions"),
		LOCTEXT("CookAndPakActionsSubMenuToolTip", "Cook And Pak actions"),
		FNewToolMenuDelegate::CreateRaw(this, &FHotPatcherEditorModule::MakeCookAndPakActionsSubMenu),
		FUIAction(
			FExecuteAction()
			),
		EUserInterfaceActionType::Button,
		false, 
		FSlateIcon(FEditorStyle::GetStyleSetName(), "ContentBrowser.SaveAllCurrentFolder")
		);
	
	// InSection.AddMenuEntry(FHotPatcherCommands::Get().AddToPakSettingsAction);
	InSection.AddDynamicEntry("AddToPatchSettgins", FNewToolMenuSectionDelegate::CreateLambda([this](FToolMenuSection& InSection)
	{
		const TAttribute<FText> Label = LOCTEXT("CookUtilities_AddToPatchSettgins", "Add To Patch Settgins");
		const TAttribute<FText> ToolTip = LOCTEXT("CookUtilities_AddToPatchSettginsTooltip", "Add Selected Assets To HotPatcher Patch Settgins");
		const FSlateIcon Icon = FSlateIcon(FEditorStyle::GetStyleSetName(), "ContentBrowser.AssetActions.Duplicate");
		const FToolMenuExecuteAction UIAction = FToolMenuExecuteAction::CreateRaw(this,&FHotPatcherEditorModule::OnAddToPatchSettings);
	
		InSection.AddMenuEntry("CookUtilities_AddToPatchSettgins", Label, ToolTip, Icon, UIAction);
	}));
	
}


void FHotPatcherEditorModule::ExtendContentBrowserAssetSelectionMenu()
{
	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu");
	FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("AssetContextCookUtilities"));//Menu->FindOrAddSection("AssetContextReferences");
	FToolMenuEntry& Entry = Section.AddDynamicEntry("AssetManagerEditorViewCommands", FNewToolMenuSectionDelegate::CreateLambda([this](FToolMenuSection& InSection)
	{
		UContentBrowserAssetContextMenuContext* Context = InSection.FindContext<UContentBrowserAssetContextMenuContext>();
		if (Context)
		{
			CreateAssetContextMenu(InSection);
		}
	}));
}

void FHotPatcherEditorModule::ExtendContentBrowserPathSelectionMenu()
{
#if ENGINE_MAJOR_VERSION > 4 || ENGINE_MINOR_VERSION > 24
	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("ContentBrowser.FolderContextMenu");
	FToolMenuSection& Section = Menu->FindOrAddSection("PathContextCookUtilities");
	FToolMenuEntry& Entry = Section.AddDynamicEntry("AssetManagerEditorViewCommands", FNewToolMenuSectionDelegate::CreateLambda([this](FToolMenuSection& InSection)
	{
		UContentBrowserFolderContext* Context = InSection.FindContext<UContentBrowserFolderContext>();
		if (Context)
		{
			CreateAssetContextMenu(InSection);
		}
	}));
#endif
}

void FHotPatcherEditorModule::MakeCookActionsSubMenu(UToolMenu* Menu)
{
	FToolMenuSection& Section = Menu->AddSection("CookActionsSection");
	UHotPatcherSettings* Settings = GetMutableDefault<UHotPatcherSettings>();
	Settings->ReloadConfig();
	
	for (auto Platform : GetAllCookPlatforms())
	{
		if(Settings->bWhiteListCookInEditor && !Settings->PlatformWhitelists.Contains(Platform))
			continue;
		Section.AddMenuEntry(
            FName(*THotPatcherTemplateHelper::GetEnumNameByValue(Platform)),
            FText::Format(LOCTEXT("Platform", "{0}"), UKismetTextLibrary::Conv_StringToText(THotPatcherTemplateHelper::GetEnumNameByValue(Platform))),
            FText(),
            FSlateIcon(),
            FUIAction(
                FExecuteAction::CreateRaw(this, &FHotPatcherEditorModule::OnCookPlatform, Platform)
            )
        );
	}
}

void FHotPatcherEditorModule::MakeCookAndPakActionsSubMenu(UToolMenu* Menu)
{
	FToolMenuSection& Section = Menu->AddSection("CookAndPakActionsSection");
	UHotPatcherSettings* Settings = GetMutableDefault<UHotPatcherSettings>();
	Settings->ReloadConfig();
	for (auto Platform : GetAllCookPlatforms())
	{
		FString PlatformName = THotPatcherTemplateHelper::GetEnumNameByValue(Platform);
		if(PlatformName.StartsWith(TEXT("All")))
			continue;
		if(Settings->bWhiteListCookInEditor && !Settings->PlatformWhitelists.Contains(Platform))
			continue;
		FToolMenuEntry& PlatformEntry = Section.AddSubMenu(FName(*PlatformName),
			FText::Format(LOCTEXT("Platform", "{0}"), UKismetTextLibrary::Conv_StringToText(THotPatcherTemplateHelper::GetEnumNameByValue(Platform))),
			FText(),
			FNewMenuDelegate::CreateLambda([=](FMenuBuilder& SubMenuBuilder){
				SubMenuBuilder.AddMenuEntry(
					LOCTEXT("AnalysisDependencies", "AnalysisDependencies"), FText(),
					FSlateIcon(FEditorStyle::GetStyleSetName(),TEXT("WorldBrowser.LevelsMenuBrush")),
					FUIAction(FExecuteAction::CreateRaw(this, &FHotPatcherEditorModule::OnCookAndPakPlatform, Platform,true)), NAME_None, EUserInterfaceActionType::Button);
				SubMenuBuilder.AddMenuEntry(
					LOCTEXT("NoDependencies", "NoDependencies"), FText(),
					FSlateIcon(FEditorStyle::GetStyleSetName(),TEXT("Level.SaveIcon16x")),
					FUIAction(FExecuteAction::CreateRaw(this, &FHotPatcherEditorModule::OnCookAndPakPlatform, Platform,false)), NAME_None, EUserInterfaceActionType::Button);
			}
			));
		
		// Section.AddMenuEntry(
  //           FName(*THotPatcherTemplateHelper::GetEnumNameByValue(Platform)),
  //           FText::Format(LOCTEXT("Platform", "{0}"), UKismetTextLibrary::Conv_StringToText(THotPatcherTemplateHelper::GetEnumNameByValue(Platform))),
  //           FText(),
  //           FSlateIcon(),
  //           FUIAction(
  //               FExecuteAction::CreateRaw(this, &FHotPatcherEditorModule::OnCookAndPakPlatform, Platform)
  //           )
        //);
	}
}

void FHotPatcherEditorModule::MakePakExternalActionsSubMenu(UToolMenu* Menu)
{
	FToolMenuSection& Section = Menu->AddSection("PakExternalActionsSection");
	UHotPatcherSettings* Settings = GetMutableDefault<UHotPatcherSettings>();
	Settings->ReloadConfig();
	for (FPakExternalInfo PakExternalConfig : Settings->PakExternalConfigs)
	{
		FString AppendPlatformName;
		for(const auto& Platform:PakExternalConfig.TargetPlatforms)
		{
			AppendPlatformName += FString::Printf(TEXT("%s/"),*THotPatcherTemplateHelper::GetEnumNameByValue(Platform));
		}
		AppendPlatformName.RemoveFromEnd(TEXT("/"));
		
		Section.AddMenuEntry(
			FName(*PakExternalConfig.PakName),
			FText::Format(LOCTEXT("PakExternal", "{0} ({1})"), UKismetTextLibrary::Conv_StringToText(PakExternalConfig.PakName),UKismetTextLibrary::Conv_StringToText(AppendPlatformName)),
			FText(),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateRaw(this, &FHotPatcherEditorModule::OnPakExternal,PakExternalConfig)
			)
		);
	}
}

void FHotPatcherEditorModule::MakeHotPatcherPresetsActionsSubMenu(UToolMenu* Menu)
{
	FToolMenuSection& Section = Menu->AddSection("HotPatcherPresetsActionsSection");
	UHotPatcherSettings* Settings = GetMutableDefault<UHotPatcherSettings>();
	Settings->ReloadConfig();
	for (FExportPatchSettings PakConfig : Settings->PresetConfigs)
	{
		Section.AddMenuEntry(
			FName(*PakConfig.VersionId),
			FText::Format(LOCTEXT("PakExternal", "VersionID: {0}"), UKismetTextLibrary::Conv_StringToText(PakConfig.VersionId)),
			FText(),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateRaw(this, &FHotPatcherEditorModule::OnPakPreset,PakConfig)
			)
		);
	}
}

void FHotPatcherEditorModule::OnAddToPatchSettings(const FToolMenuContext& MenuContent)
{
	OpenDockTab();
	
	TArray<FAssetData> AssetsData = GetSelectedAssetsInBrowserContent();
	TArray<FString> FolderList = GetSelectedFolderInBrowserContent();
	TArray<FDirectoryPath> FolderDirectorys;

	for(const auto& Folder:FolderList)
	{
		FDirectoryPath Path;
		Path.Path = Folder;
		FolderDirectorys.Add(Path);
	}
	TArray<FPatcherSpecifyAsset> AssetsSoftPath;

	for(const auto& AssetData:AssetsData)
	{
		FPatcherSpecifyAsset PatchSettingAssetElement;
		FSoftObjectPath AssetObjectPath;
		AssetObjectPath.SetPath(AssetData.ObjectPath.ToString());
		PatchSettingAssetElement.Asset = AssetObjectPath;
		AssetsSoftPath.AddUnique(PatchSettingAssetElement);
	}
	GPatchSettings->IncludeSpecifyAssets.Append(AssetsSoftPath);
	GPatchSettings->AssetIncludeFilters.Append(FolderDirectorys);
}

void FHotPatcherEditorModule::OnPakPreset(FExportPatchSettings Config)
{
	UHotPatcherSettings* Settings = GetMutableDefault<UHotPatcherSettings>();
	Settings->ReloadConfig();
	
	UPatcherProxy* PatcherProxy = NewObject<UPatcherProxy>();
	PatcherProxy->AddToRoot();
	Proxys.Add(PatcherProxy);
	
	PatcherProxy->SetProxySettings(&Config);

	if(!Config.IsStandaloneMode())
	{
		PatcherProxy->DoExport();
	}
	else
	{
		FString CurrentConfig;
		THotPatcherTemplateHelper::TSerializeStructAsJsonString(Config,CurrentConfig);
		FString SaveConfigTo = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("HotPatcher"),TEXT("PatchConfig.json")));
		FFileHelper::SaveStringToFile(CurrentConfig,*SaveConfigTo);
		FString MissionCommand = FString::Printf(TEXT("\"%s\" -run=HotPatcher -config=\"%s\" %s"),*UFlibPatchParserHelper::GetProjectFilePath(),*SaveConfigTo,*Config.GetCombinedAdditionalCommandletArgs());
		UE_LOG(LogHotPatcher,Log,TEXT("HotPatcher %s Mission: %s %s"),*Config.VersionId,*UFlibHotPatcherEditorHelper::GetUECmdBinary(),*MissionCommand);
		RunProcMission(UFlibHotPatcherEditorHelper::GetUECmdBinary(),MissionCommand,FString::Printf(TEXT("Mission: %s"),*Config.VersionId));
	}
}

#endif

void FHotPatcherEditorModule::OnCookPlatform(ETargetPlatform Platform)
{
	UHotPatcherSettings* Settings = GetMutableDefault<UHotPatcherSettings>();
	Settings->ReloadConfig();
	TArray<FAssetData> AssetsData = GetSelectedAssetsInBrowserContent();
	TArray<FString> SelectedFolder = GetSelectedFolderInBrowserContent();
	TArray<FAssetData> FolderAssetDatas;
	
	UFlibAssetManageHelper::GetAssetDataInPaths(SelectedFolder,FolderAssetDatas);
	AssetsData.Append(FolderAssetDatas);
	
	FCookManager::FCookMission CookMission;
	
	auto CookNotifyLambda = [](ETargetPlatform Platform,const FString& PackageName,const FString& CookedSavePath)
	{
		bool bSuccessed = true;
		auto Msg = FText::Format(
			LOCTEXT("CookAssetsNotify", "Cook {1} for {0} {2}!"),
			UKismetTextLibrary::Conv_StringToText(PackageName),
			UKismetTextLibrary::Conv_StringToText(THotPatcherTemplateHelper::GetEnumNameByValue(Platform)),
			bSuccessed ? UKismetTextLibrary::Conv_StringToText(TEXT("Successfuly")):UKismetTextLibrary::Conv_StringToText(TEXT("Faild"))
			);
		SNotificationItem::ECompletionState CookStatus = bSuccessed ? SNotificationItem::CS_Success:SNotificationItem::CS_Fail;
		UFlibHotPatcherEditorHelper::CreateSaveFileNotify(Msg,CookedSavePath,CookStatus);
	};
	
	for(int32 index=0;index < AssetsData.Num();++index)
	{
		FCookManager::FCookPackageInfo CurrentPackage;
		CurrentPackage.AssetData = AssetsData[index];
		CurrentPackage.PackageName = AssetsData[index].PackageName.ToString();
		CurrentPackage.CookPlatforms = TArray<ETargetPlatform>{Platform};
		CurrentPackage.Callback = CookNotifyLambda;
		CookMission.MissionPackages.Add(CurrentPackage);
		if(!GCookLog)
		{
			UE_LOG(LogHotPatcher,Log,TEXT("Cook %s for %s"),*AssetsData[index].PackagePath.ToString(),*THotPatcherTemplateHelper::GetEnumNameByValue(Platform));
		}
	}
	CookMission.Callback = [](bool){};
	TFunction<void(TArray<FCookManager::FCookPackageInfo>)> OnCookFaildLambda = [](TArray<FCookManager::FCookPackageInfo> FaildPackages)
	{
		for(const auto& Package:FaildPackages)
		{
			FString PlatformsStr;
			for(const auto& Platform:Package.CookPlatforms)
			{
				PlatformsStr += FString::Printf(TEXT("%s/"),*THotPatcherTemplateHelper::GetEnumNameByValue(Platform));
			}
			PlatformsStr.RemoveFromEnd(TEXT("/"));
			auto Msg = FText::Format(
						LOCTEXT("CookAssetsNotify", "Cook {1} for {0} {2}!"),
						UKismetTextLibrary::Conv_StringToText(Package.PackageName),
						UKismetTextLibrary::Conv_StringToText(PlatformsStr),
						UKismetTextLibrary::Conv_StringToText(TEXT("Faild"))
						);
			UFlibHotPatcherEditorHelper::CreateSaveFileNotify(Msg,Package.PackageName,SNotificationItem::ECompletionState::CS_Fail);
		}
	};
	FCookManager::Get().AddCookMission(CookMission,OnCookFaildLambda);

}

void FHotPatcherEditorModule::OnPakExternal(FPakExternalInfo PakExternConfig)
{
	PatchSettings = MakeShareable(new FExportPatchSettings);
	*PatchSettings = MakeTempPatchSettings(
		PakExternConfig.PakName,
		TArray<FDirectoryPath>{},
		TArray<FPatcherSpecifyAsset>{},
		TArray<FPlatformExternAssets>{PakExternConfig.AddExternAssetsToPlatform},
		PakExternConfig.TargetPlatforms,
		false
	);

	UPatcherProxy* PatcherProxy = NewObject<UPatcherProxy>();
	PatcherProxy->SetProxySettings(PatchSettings.Get());

	PatcherProxy->DoExport();
}

void FHotPatcherEditorModule::CookAndPakByAssetsAndFilters(TArray<FPatcherSpecifyAsset> IncludeAssets,TArray<FDirectoryPath> IncludePaths,TArray<ETargetPlatform> Platforms,bool bForceStandalone)
{
	FString Name = FDateTime::Now().ToString();
	PatchSettings = MakeShareable(new FExportPatchSettings);
	*PatchSettings = MakeTempPatchSettings(Name,IncludePaths,IncludeAssets,TArray<FPlatformExternAssets>{},Platforms,true);

	UPatcherProxy* PatcherProxy = NewObject<UPatcherProxy>();
	PatcherProxy->AddToRoot();
	Proxys.Add(PatcherProxy);
	PatcherProxy->SetProxySettings(PatchSettings.Get());

	if(bForceStandalone || PatchSettings->IsStandaloneMode())
	{
		FString CurrentConfig;
		THotPatcherTemplateHelper::TSerializeStructAsJsonString(*PatchSettings,CurrentConfig);
		FString SaveConfigTo = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("HotPatcher"),TEXT("PatchConfig.json")));
		FFileHelper::SaveStringToFile(CurrentConfig,*SaveConfigTo);
		FString MissionCommand = FString::Printf(TEXT("\"%s\" -run=HotPatcher -config=\"%s\" %s"),*UFlibPatchParserHelper::GetProjectFilePath(),*SaveConfigTo,*PatchSettings->GetCombinedAdditionalCommandletArgs());
		UE_LOG(LogHotPatcher,Log,TEXT("HotPatcher %s Mission: %s %s"),*PatchSettings->VersionId,*UFlibHotPatcherEditorHelper::GetUECmdBinary(),*MissionCommand);
		RunProcMission(UFlibHotPatcherEditorHelper::GetUECmdBinary(),MissionCommand,FString::Printf(TEXT("Mission: %s"),*PatchSettings->VersionId));
	}
	else
	{
		PatcherProxy->DoExport();
	}
}

void FHotPatcherEditorModule::OnCookAndPakPlatform(ETargetPlatform Platform, bool bAnalysicDependencies)
{
	UHotPatcherSettings* Settings = GetMutableDefault<UHotPatcherSettings>();
	Settings->ReloadConfig();
	
	TArray<FAssetData> AssetsDatas = GetSelectedAssetsInBrowserContent();
	TArray<FString> SelectedFolder = GetSelectedFolderInBrowserContent();
	TArray<FDirectoryPath> IncludePaths;
	TArray<FPatcherSpecifyAsset> IncludeAssets;
	
	for(auto& Folder:SelectedFolder)
	{
		FDirectoryPath CurrentPath;
		if(Folder.StartsWith(TEXT("/All/")))
		{
			Folder.RemoveFromStart(TEXT("/All"));
		}
		CurrentPath.Path = Folder;
		IncludePaths.Add(CurrentPath);
	}

	for(const auto& Asset:AssetsDatas)
	{
		FPatcherSpecifyAsset CurrentAsset;
		CurrentAsset.Asset = Asset.ToSoftObjectPath();
		CurrentAsset.bAnalysisAssetDependencies = bAnalysicDependencies;
		CurrentAsset.AssetRegistryDependencyTypes.AddUnique(EAssetRegistryDependencyTypeEx::Packages);
		IncludeAssets.AddUnique(CurrentAsset);
	}
	CookAndPakByAssetsAndFilters(IncludeAssets,IncludePaths,TArray<ETargetPlatform>{Platform});
}

void FHotPatcherEditorModule::OnObjectSaved(UObject* ObjectSaved)
{
	if (GIsCookerLoadingPackage)
	{
		// This is the cooker saving a cooked package, ignore
		return;
	}

	UPackage* Package = ObjectSaved->GetOutermost();
	if (Package == nullptr || Package == GetTransientPackage())
	{
		return;
	}


	// Register the package filename as modified. We don't use the cache because the file may not exist on disk yet at this point
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	AssetRegistryModule.Get().ScanFilesSynchronous(TArray<FString>{PackageFilename},false);
}

FExportPatchSettings FHotPatcherEditorModule::MakeTempPatchSettings(
	const FString& Name,
	const TArray<FDirectoryPath>& AssetIncludeFilters,
	const TArray<FPatcherSpecifyAsset>& IncludeSpecifyAssets,
	const TArray<FPlatformExternAssets>& ExternFiles,
	const TArray<ETargetPlatform>& PakTargetPlatforms,
	bool bCook
)
{
	UHotPatcherSettings* Settings = GetMutableDefault<UHotPatcherSettings>();
	Settings->ReloadConfig();
	FExportPatchSettings TempSettings = Settings->TempPatchSetting;
	TempSettings.VersionId = Name;
	TempSettings.AssetIncludeFilters = AssetIncludeFilters;
	TempSettings.IncludeSpecifyAssets = IncludeSpecifyAssets;
	TempSettings.AddExternAssetsToPlatform = ExternFiles;
	TempSettings.bCookPatchAssets = bCook;
	TempSettings.PakTargetPlatforms = PakTargetPlatforms;
	TempSettings.SavePath.Path = Settings->GetTempSavedDir();
	return TempSettings;
}


TArray<ETargetPlatform> FHotPatcherEditorModule::GetAllCookPlatforms() const
{
	TArray<ETargetPlatform> TargetPlatforms;//{ETargetPlatform::Android_ASTC,ETargetPlatform::IOS,ETargetPlatform::WindowsNoEditor};
	#if ENGINE_MAJOR_VERSION > 4 || ENGINE_MINOR_VERSION > 21
	UEnum* FoundEnum = StaticEnum<ETargetPlatform>();
#else
	FString EnumTypeName = ANSI_TO_TCHAR(UFlibPatchParserHelper::GetCPPTypeName<ETargetPlatform>().c_str());
	UEnum* FoundEnum = FindObject<UEnum>(ANY_PACKAGE, *EnumTypeName, true); 
#endif
	if (FoundEnum)
	{
		for(int32 index =1;index < FoundEnum->GetMaxEnumValue();++index)
		{
			TargetPlatforms.AddUnique((ETargetPlatform)(index));
		}
			
	}
	return TargetPlatforms;
}

TSharedPtr<FProcWorkerThread> FHotPatcherEditorModule::RunProcMission(const FString& Bin, const FString& Command, const FString& MissionName)
{
	if (mProcWorkingThread.IsValid() && mProcWorkingThread->GetThreadStatus()==EThreadStatus::Busy)
	{
		mProcWorkingThread->Cancel();
	}
	else
	{
		mProcWorkingThread = MakeShareable(new FProcWorkerThread(*FString::Printf(TEXT("PakPresetThread_%s"),*MissionName), Bin, Command));
		mProcWorkingThread->ProcOutputMsgDelegate.BindUObject(MissionNotifyProay,&UMissionNotificationProxy::ReceiveOutputMsg);
		mProcWorkingThread->ProcBeginDelegate.AddUObject(MissionNotifyProay,&UMissionNotificationProxy::SpawnRuningMissionNotification);
		mProcWorkingThread->ProcSuccessedDelegate.AddUObject(MissionNotifyProay,&UMissionNotificationProxy::SpawnMissionSuccessedNotification);
		mProcWorkingThread->ProcFaildDelegate.AddUObject(MissionNotifyProay,&UMissionNotificationProxy::SpawnMissionFaildNotification);
		MissionNotifyProay->SetMissionName(*FString::Printf(TEXT("%s"),*MissionName));
		MissionNotifyProay->SetMissionNotifyText(
			FText::FromString(FString::Printf(TEXT("%s in progress"),*MissionName)),
			LOCTEXT("RunningCookNotificationCancelButton", "Cancel"),
			FText::FromString(FString::Printf(TEXT("%s Mission Finished!"),*MissionName)),
			FText::FromString(FString::Printf(TEXT("%s Failed!"),*MissionName))
		);
		MissionNotifyProay->MissionCanceled.AddLambda([this]()
		{
			if (mProcWorkingThread.IsValid() && mProcWorkingThread->GetThreadStatus() == EThreadStatus::Busy)
			{
				mProcWorkingThread->Cancel();
			}
		});
		
		mProcWorkingThread->Execute();
	}
	return mProcWorkingThread;
}
#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FHotPatcherEditorModule, HotPatcherEditor)