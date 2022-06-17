// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SConcertClientBrowserItem.h"
#include "Misc/TextFilter.h"
#include "Models/IClientBrowserModel.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class FConcertClientsTabController;
class STableViewBase;
class ITableRow;
template<typename T> class STileView;

enum class EConcertClientStatus : uint8;
struct FConcertSessionClientInfo;

namespace UE::MultiUserServer
{
	class IClientNetworkStatisticsModel;
	class IClientBrowserModel;
	struct FClientBrowserItem;
	
	/**
	 * Presents a view of client thumbnails, which include important statistics.
	 * Clients can be right-clicked, opening a context menu, and double clicked, opening a new tab with a log view.
	 */
	class SConcertClientBrowser : public SCompoundWidget
	{
	public:

		DECLARE_DELEGATE_OneParam(FOnClientDoubleClicked, const FGuid& /*EndpointId*/);

		SLATE_BEGIN_ARGS(SConcertClientBrowser) {}
			/** Extension point to the right of the search bar */
			SLATE_NAMED_SLOT(FArguments, RightOfSearch)
			SLATE_EVENT(FOnClientDoubleClicked, OnClientDoubleClicked)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, TSharedRef<IClientBrowserModel> InBrowserModel, TSharedRef<IClientNetworkStatisticsModel> InStatisticsModel);

		/** Shows only the clients connected to the given session ID */
		void ShowOnlyClientsFromSession(const FGuid& SessionId);
		
	private:

		using FSessionId = FGuid;
		using FMessagingNodeId = FGuid;
		using FClientTextFilter = TTextFilter<const TSharedPtr<FClientBrowserItem>&>;

		/** Retrieves clients and live sessions */
		TSharedPtr<IClientBrowserModel> BrowserModel;
		/** Used by sub-widgets to display networking data */
		TSharedPtr<IClientNetworkStatisticsModel> StatisticsModel;

		/** Contains the guid of every session we're allowed to display */
		TSet<FSessionId> AllowedSessions;
		/** Should all sessions be shown? */
		bool bShowAllSessions = true;
		/** Should admin endpoints be shown? */
		bool bShowSessionlessClients = true;
		/** Keeps widgets alive even when not displayed in the view - otherwise the graph will lose the historic data. */
		TMap<FMessagingNodeId, TSharedPtr<SConcertClientBrowserItem>> ClientWidgets;

		/** Source array for TileView - fitlered version of IClientBrowserModel::GetItems */
		TArray<TSharedPtr<FClientBrowserItem>> DisplayedClients;
		/** Visualizes all the items */
		TSharedPtr<STileView<TSharedPtr<FClientBrowserItem>>> TileView;

		// Filtering
		TSharedPtr<FText> HighlightText;
		TSharedPtr<FClientTextFilter> SessionFilter;
		
		FOnClientDoubleClicked OnClientDoubleClicked;

		TSharedRef<SWidget> CreateSearchArea(const FArguments& InArgs);
		TSharedRef<SWidget> CreateKeepDisconnectedClients();
		TSharedRef<SWidget> CreateTileView();

		// Model events
		void OnSessionCreated(const FGuid& SessionId);
		void OnSessionDestroyed(const FGuid& SessionId);
		void OnClientListChanged(TSharedPtr<FClientBrowserItem> Item, IClientBrowserModel::EClientUpdateType UpdateType);

		// Combo button
		TSharedRef<SWidget> MakeSessionOption();

		FText GetErrorMessageText() const;
		
		// TileView events
		TSharedRef<ITableRow> MakeTileViewWidget(TSharedPtr<FClientBrowserItem> ClientItem, const TSharedRef<STableViewBase>& OwnerTable);
		TSharedPtr<SWidget> OnGetContextMenuContent();
		void OnListMouseButtonDoubleClick(TSharedPtr<FClientBrowserItem> ClientItem);

		// Filtering
		void AllowAllSessions();
		void DisallowAllSessions();
		void UpdateTileViewFromAllowedSessions();
		bool PassesFilter(const TSharedPtr<FClientBrowserItem>& Client) const;
		void GenerateSearchTerms(const TSharedPtr<FClientBrowserItem>& Client, TArray<FString>& SearchTerms) const;
	};
}


