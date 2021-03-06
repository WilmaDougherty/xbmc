/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://kodi.tv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "threads/SystemClock.h"
#include "GUIUserMessages.h"
#include "GUIWindowMusicBase.h"
#include "dialogs/GUIDialogMediaSource.h"
#include "dialogs/GUIDialogFileBrowser.h"
#include "music/dialogs/GUIDialogInfoProviderSettings.h"
#include "music/dialogs/GUIDialogMusicInfo.h"
#include "playlists/PlayListFactory.h"
#include "Util.h"
#include "playlists/PlayListM3U.h"
#include "Application.h"
#include "PlayListPlayer.h"
#include "ServiceBroker.h"
#ifdef HAS_CDDA_RIPPER
#include "cdrip/CDDARipper.h"
#endif
#include "GUIPassword.h"
#include "PartyModeManager.h"
#include "GUIInfoManager.h"
#include "filesystem/Directory.h"
#include "filesystem/MusicDatabaseDirectory.h"
#include "music/dialogs/GUIDialogSongInfo.h"
#include "addons/GUIDialogAddonInfo.h"
#include "dialogs/GUIDialogSmartPlaylistEditor.h"
#include "view/GUIViewState.h"
#include "music/tags/MusicInfoTag.h"
#include "guilib/GUIWindowManager.h"
#include "input/Key.h"
#include "dialogs/GUIDialogYesNo.h"
#include "dialogs/GUIDialogProgress.h"
#include "FileItem.h"
#include "filesystem/File.h"
#include "messaging/helpers/DialogHelper.h"
#include "messaging/helpers/DialogOKHelper.h"
#include "profiles/ProfilesManager.h"
#include "storage/MediaManager.h"
#include "settings/AdvancedSettings.h"
#include "settings/MediaSettings.h"
#include "settings/MediaSourceSettings.h"
#include "settings/Settings.h"
#include "guilib/LocalizeStrings.h"
#include "utils/log.h"
#include "utils/URIUtils.h"
#include "utils/StringUtils.h"
#include "utils/Variant.h"
#include "URL.h"
#include "music/infoscanner/MusicInfoScanner.h"
#include "guiinfo/GUIInfoLabels.h"
#include "cores/IPlayer.h"
#include "cores/playercorefactory/PlayerCoreFactory.h"
#include "CueDocument.h"
#include "Autorun.h"

#ifdef TARGET_POSIX
#include "platform/linux/XTimeUtils.h"
#endif

using namespace XFILE;
using namespace MUSICDATABASEDIRECTORY;
using namespace PLAYLIST;
using namespace MUSIC_GRABBER;
using namespace MUSIC_INFO;
using namespace KODI::MESSAGING;
using KODI::MESSAGING::HELPERS::DialogResponse;

#define CONTROL_BTNVIEWASICONS  2
#define CONTROL_BTNSORTBY       3
#define CONTROL_BTNSORTASC      4
#define CONTROL_BTNPLAYLISTS    7
#define CONTROL_BTNSCAN         9
#define CONTROL_BTNRIP          11

CGUIWindowMusicBase::CGUIWindowMusicBase(int id, const std::string &xmlFile)
    : CGUIMediaWindow(id, xmlFile.c_str())
{
  m_dlgProgress = NULL;
  m_thumbLoader.SetObserver(this);
}

CGUIWindowMusicBase::~CGUIWindowMusicBase () = default;

bool CGUIWindowMusicBase::OnBack(int actionID)
{
  if (!g_application.IsMusicScanning())
  {
    CUtil::RemoveTempFiles();
  }
  return CGUIMediaWindow::OnBack(actionID);
}

/*!
 \brief Handle messages on window.
 \param message GUI Message that can be reacted on.
 \return if a message can't be processed, return \e false

 On these messages this class reacts.\n
 When retrieving...
  - #GUI_MSG_WINDOW_DEINIT\n
   ...the last focused control is saved to m_iLastControl.
  - #GUI_MSG_WINDOW_INIT\n
   ...the musicdatabase is opend and the music extensions and shares are set.
   The last focused control is set.
  - #GUI_MSG_CLICKED\n
   ... the base class reacts on the following controls:\n
    Buttons:\n
    - #CONTROL_BTNVIEWASICONS - switch between list, thumb and with large items
    - #CONTROL_BTNSEARCH - Search for items\n
    Other Controls:
    - The container controls\n
     Have the following actions in message them clicking on them.
     - #ACTION_QUEUE_ITEM - add selected item to playlist
     - #ACTION_SHOW_INFO - retrieve album info from the internet
     - #ACTION_SELECT_ITEM - Item has been selected. Overwrite OnClick() to react on it
 */
bool CGUIWindowMusicBase::OnMessage(CGUIMessage& message)
{
  switch ( message.GetMessage() )
  {
  case GUI_MSG_WINDOW_DEINIT:
    {
      if (m_thumbLoader.IsLoading())
        m_thumbLoader.StopThread();
      m_musicdatabase.Close();
    }
    break;

  case GUI_MSG_WINDOW_INIT:
    {
      m_dlgProgress = g_windowManager.GetWindow<CGUIDialogProgress>(WINDOW_DIALOG_PROGRESS);

      m_musicdatabase.Open();

      if (!CGUIMediaWindow::OnMessage(message))
        return false;

      return true;
    }
    break;
  case GUI_MSG_DIRECTORY_SCANNED:
    {
      CFileItem directory(message.GetStringParam(), true);

      // Only update thumb on a local drive
      if (directory.IsHD())
      {
        std::string strParent;
        URIUtils::GetParentPath(directory.GetPath(), strParent);
        if (directory.GetPath() == m_vecItems->GetPath() || strParent == m_vecItems->GetPath())
          Refresh();
      }
    }
    break;

  // update the display
  case GUI_MSG_SCAN_FINISHED:
  case GUI_MSG_REFRESH_THUMBS:
    Refresh();
    break;

  case GUI_MSG_CLICKED:
    {
      int iControl = message.GetSenderId();
      if (iControl == CONTROL_BTNRIP)
      {
        OnRipCD();
      }
      else if (iControl == CONTROL_BTNPLAYLISTS)
      {
        if (!m_vecItems->IsPath("special://musicplaylists/"))
          Update("special://musicplaylists/");
      }
      else if (iControl == CONTROL_BTNSCAN)
      {
        OnScan(-1);
      }
      else if (m_viewControl.HasControl(iControl))  // list/thumb control
      {
        int iItem = m_viewControl.GetSelectedItem();
        int iAction = message.GetParam1();

        // iItem is checked for validity inside these routines
        if (iAction == ACTION_QUEUE_ITEM || iAction == ACTION_MOUSE_MIDDLE_CLICK)
        {
          OnQueueItem(iItem);
        }
        else if (iAction == ACTION_SHOW_INFO)
        {
          OnItemInfo(iItem);
        }
        else if (iAction == ACTION_DELETE_ITEM)
        {
          // is delete allowed?
          // must be at the playlists directory
          if (m_vecItems->IsPath("special://musicplaylists/"))
            OnDeleteItem(iItem);

          else
            return false;
        }
        // use play button to add folders of items to temp playlist
        else if (iAction == ACTION_PLAYER_PLAY)
        {
          // if playback is paused or playback speed != 1, return
          if (g_application.GetAppPlayer().IsPlayingAudio())
          {
            if (g_application.GetAppPlayer().IsPausedPlayback())
              return false;
            if (g_application.GetAppPlayer().GetPlaySpeed() != 1)
              return false;
          }

          // not playing audio, or playback speed == 1
          PlayItem(iItem);

          return true;
        }
      }
    }
    break;
  case GUI_MSG_NOTIFY_ALL:
    {
      if (message.GetParam1()==GUI_MSG_REMOVED_MEDIA)
        CUtil::DeleteDirectoryCache("r-");
    }
    break;
  }
  return CGUIMediaWindow::OnMessage(message);
}

bool CGUIWindowMusicBase::OnAction(const CAction &action)
{
  if (action.GetID() == ACTION_SHOW_PLAYLIST)
  {
    if (CServiceBroker::GetPlaylistPlayer().GetCurrentPlaylist() == PLAYLIST_MUSIC ||
        CServiceBroker::GetPlaylistPlayer().GetPlaylist(PLAYLIST_MUSIC).size() > 0)
    {
      g_windowManager.ActivateWindow(WINDOW_MUSIC_PLAYLIST);
      return true;
    }
  }

  if (action.GetID() == ACTION_SCAN_ITEM)
  {
    int item = m_viewControl.GetSelectedItem();
    if (item > -1 && m_vecItems->Get(item)->m_bIsFolder)
      OnScan(item);

    return true;
  }

  return CGUIMediaWindow::OnAction(action);
}

void CGUIWindowMusicBase::OnItemInfoAll(const std::string strPath, bool refresh )
{
  if (StringUtils::EqualsNoCase(m_vecItems->GetContent(), "albums"))
    g_application.StartMusicAlbumScan(strPath, refresh);
  else if (StringUtils::EqualsNoCase(m_vecItems->GetContent(), "artists"))
    g_application.StartMusicArtistScan(strPath, refresh);
}

/// \brief Retrieves music info for albums from allmusic.com and displays them in CGUIDialogMusicInfo
/// \param iItem Item in list/thumb control
void CGUIWindowMusicBase::OnItemInfo(int iItem, bool bShowInfo)
{
  if ( iItem < 0 || iItem >= m_vecItems->Size() )
    return;

  CFileItemPtr item = m_vecItems->Get(iItem);

  if (item->IsVideoDb())
  { // music video
    OnContextButton(iItem, CONTEXT_BUTTON_INFO);
    return;
  }

  if (!m_vecItems->IsPlugin() && (item->IsPlugin() || item->IsScript()))
  {
    CGUIDialogAddonInfo::ShowForItem(item);
    return;
  }

  OnItemInfo(item.get(), bShowInfo);
}

void CGUIWindowMusicBase::OnItemInfo(CFileItem *pItem, bool bShowInfo)
{
  if ((pItem->IsMusicDb() && !pItem->HasMusicInfoTag()) || pItem->IsParentFolder() ||
       URIUtils::IsSpecial(pItem->GetPath()) || StringUtils::StartsWithNoCase(pItem->GetPath(), "musicsearch://"))
    return; // nothing to do

  if (!pItem->m_bIsFolder)
  { // song lookup
    ShowSongInfo(pItem);
    return;
  }

  // this function called from outside this window - make sure the database is open
  m_musicdatabase.Open();

  // we have a folder
  if (pItem->IsMusicDb())
  {
    CQueryParams params;
    CDirectoryNode::GetDatabaseInfo(pItem->GetPath(), params);
    if (params.GetAlbumId() == -1)
      ShowArtistInfo(pItem);
    else
      ShowAlbumInfo(pItem);

    if (m_dlgProgress && bShowInfo)
      m_dlgProgress->Close();
    return;
  }

  int albumID = m_musicdatabase.GetAlbumIdByPath(pItem->GetPath());
  if (albumID != -1)
  {
    CAlbum album;
    if (!m_musicdatabase.GetAlbum(albumID, album))
      return;
    CFileItem item(StringUtils::Format("musicdb://albums/%i/", albumID), album);
    if (ShowAlbumInfo(&item))
      return;
  }

  CLog::Log(LOGINFO, "%s called on a folder containing no songs in the library - nothing can be done", __FUNCTION__);
}

void CGUIWindowMusicBase::ShowArtistInfo(const CFileItem *pItem, bool bShowInfo /* = true */)
{
  CQueryParams params;
  CDirectoryNode::GetDatabaseInfo(pItem->GetPath(), params);

  ADDON::ScraperPtr scraper;
  if (!m_musicdatabase.GetScraper(params.GetArtistId(), CONTENT_ARTISTS, scraper))
    return;

  CArtist artist;
  if (!m_musicdatabase.GetArtist(params.GetArtistId(), artist))
    return;
  // Get the *name* of the folder for this artist within the Artist Info folder (may not exist).
  // If there is no Artist Info folder specififed in settings this will be blank
  bool artistpathfound = m_musicdatabase.GetArtistPath(artist, artist.strPath);

  // Set up path for *item folder when browsing for art, by default this is in the Artist Info Folder
  std::string artistItemPath = artist.strPath;
  if (!artistpathfound || !CDirectory::Exists(artist.strPath))
    // Fall back local to music files (historic location for those album artists with a unique folder)
    // although there may not be such a unique folder for the arist
    if (!m_musicdatabase.GetOldArtistPath(artist.idArtist, artistItemPath))
      // Fall back further to browse the Artist Info Folder itself
      artistItemPath = CServiceBroker::GetSettings().GetString(CSettings::SETTING_MUSICLIBRARY_ARTISTSFOLDER);

  bool refresh = false;
  while (1)
  {
    // Check if the entry should be refreshed (Only happens if a user pressed refresh)
    if (refresh)
    {
      const CProfilesManager &profileManager = CServiceBroker::GetProfileManager();

      if (!profileManager.GetCurrentProfile().canWriteDatabases() && !g_passwordManager.bMasterUser)
        break; // should display a dialog saying no permissions

      if (g_application.IsMusicScanning())
      {
        HELPERS::ShowOKDialogText(CVariant{189}, CVariant{14057});
        break;
      }

      // show dialog box indicating we're searching the album
      if (m_dlgProgress && bShowInfo)
      {
        m_dlgProgress->SetHeading(CVariant{21889});
        m_dlgProgress->SetLine(0, CVariant{pItem->GetMusicInfoTag()->GetArtist()});
        m_dlgProgress->SetLine(1, CVariant{""});
        m_dlgProgress->SetLine(2, CVariant{""});
        m_dlgProgress->Open();
      }

      CMusicInfoScanner scanner;
      if (scanner.UpdateArtistInfo(artist, scraper, bShowInfo, m_dlgProgress) != CInfoScanner::INFO_ADDED)
      {
        HELPERS::ShowOKDialogText(CVariant{21889}, CVariant{20199});
        break;
      }
    }

    if (m_dlgProgress)
      m_dlgProgress->Close();

    CGUIDialogMusicInfo *pDlgArtistInfo = g_windowManager.GetWindow<CGUIDialogMusicInfo>(WINDOW_DIALOG_MUSIC_INFO);
    if (pDlgArtistInfo)
    {
      pDlgArtistInfo->SetArtist(artist, artistItemPath);
      pDlgArtistInfo->Open();

      if (pDlgArtistInfo->NeedRefresh())
      {
        m_musicdatabase.ClearArtistLastScrapedTime(params.GetArtistId());
        refresh = true;
        continue;
      } 
      else if (pDlgArtistInfo->HasUpdatedThumb()) 
      {
        Update(m_vecItems->GetPath());
      }
    }
    break;
  }
  if (m_dlgProgress)
    m_dlgProgress->Close();
}

bool CGUIWindowMusicBase::ShowAlbumInfo(const CFileItem *pItem, bool bShowInfo /* = true */)
{
  CQueryParams params;
  CDirectoryNode::GetDatabaseInfo(pItem->GetPath(), params);

  ADDON::ScraperPtr scraper;
  if (!m_musicdatabase.GetScraper(params.GetAlbumId(), CONTENT_ALBUMS, scraper))
    return false;

  CAlbum album;
  if (!m_musicdatabase.GetAlbum(params.GetAlbumId(), album))
    return false;
  m_musicdatabase.GetAlbumPath(params.GetAlbumId(), album.strPath);
  bool refresh = false;
  while (1)
  {
    // Check if the entry should be refreshed (Only happens if a user pressed refresh)
    if (refresh)
    {
      const CProfilesManager &profileManager = CServiceBroker::GetProfileManager();

      if (!profileManager.GetCurrentProfile().canWriteDatabases() && !g_passwordManager.bMasterUser)
      {
        //! @todo should display a dialog saying no permissions
        if (m_dlgProgress)
          m_dlgProgress->Close();
        return false;
      }

      if (g_application.IsMusicScanning())
      {
        HELPERS::ShowOKDialogText(CVariant{189}, CVariant{14057});
        if (m_dlgProgress)
          m_dlgProgress->Close();
        return false;
      }

      // show dialog box indicating we're searching the album
      if (m_dlgProgress && bShowInfo)
      {
        m_dlgProgress->SetHeading(CVariant{185});
        m_dlgProgress->SetLine(0, CVariant{pItem->GetMusicInfoTag()->GetAlbum()});
        m_dlgProgress->SetLine(1, CVariant{pItem->GetMusicInfoTag()->GetAlbumArtistString()});
        m_dlgProgress->SetLine(2, CVariant{""});
        m_dlgProgress->Open();
      }

      CMusicInfoScanner scanner;
      if (scanner.UpdateAlbumInfo(album, scraper, bShowInfo, m_dlgProgress) != CInfoScanner::INFO_ADDED)
      {
        HELPERS::ShowOKDialogText(CVariant{185}, CVariant{500});
        if (m_dlgProgress)
          m_dlgProgress->Close();
        return false;
      }
    }

    if (m_dlgProgress)
      m_dlgProgress->Close();

    CGUIDialogMusicInfo *pDlgAlbumInfo = g_windowManager.GetWindow<CGUIDialogMusicInfo>(WINDOW_DIALOG_MUSIC_INFO);
    if (pDlgAlbumInfo)
    {
      pDlgAlbumInfo->SetAlbum(album, album.strPath);
      pDlgAlbumInfo->Open();

      if (pDlgAlbumInfo->NeedRefresh())
      {
        m_musicdatabase.ClearAlbumLastScrapedTime(params.GetAlbumId());
        refresh = true;
        continue;
      }
      else if (pDlgAlbumInfo->HasUpdatedThumb())
        UpdateThumb(album, album.strPath);
      else if (pDlgAlbumInfo->NeedsUpdate())
        Refresh(true); // update our file list

    }
    break;
  }
  if (m_dlgProgress)
    m_dlgProgress->Close();
  return true;
}

void CGUIWindowMusicBase::ShowSongInfo(CFileItem* pItem)
{
  CGUIDialogSongInfo *dialog = g_windowManager.GetWindow<CGUIDialogSongInfo>(WINDOW_DIALOG_SONG_INFO);
  if (dialog)
  {
    if (!pItem->IsMusicDb())
      pItem->LoadMusicTag();
    if (!pItem->HasMusicInfoTag())
      return;

    dialog->SetSong(pItem);
    dialog->Open();
    if (dialog->NeedsUpdate())
      Refresh(true); // update our file list
  }
}

/*
/// \brief Can be overwritten to implement an own tag filling function.
/// \param items File items to fill
void CGUIWindowMusicBase::OnRetrieveMusicInfo(CFileItemList& items)
{
}
*/

/// \brief Retrieve tag information for \e m_vecItems
void CGUIWindowMusicBase::RetrieveMusicInfo()
{
  unsigned int startTick = XbmcThreads::SystemClockMillis();

  OnRetrieveMusicInfo(*m_vecItems);

  //! @todo Scan for multitrack items here...
  std::vector<std::string> itemsForRemove;
  CFileItemList itemsForAdd;
  for (int i = 0; i < m_vecItems->Size(); ++i)
  {
    CFileItemPtr pItem = (*m_vecItems)[i];
    if (pItem->m_bIsFolder || pItem->IsPlayList() || pItem->IsPicture() || pItem->IsLyrics())
      continue;

    CMusicInfoTag& tag = *pItem->GetMusicInfoTag();
    if (tag.Loaded() && !tag.GetCueSheet().empty())
      pItem->LoadEmbeddedCue();

    if (pItem->HasCueDocument()
      && pItem->LoadTracksFromCueDocument(itemsForAdd))
    {
      itemsForRemove.push_back(pItem->GetPath());
    }
  }
  for (size_t i = 0; i < itemsForRemove.size(); ++i)
  {
    for (int j = 0; j < m_vecItems->Size(); ++j)
    {
      if ((*m_vecItems)[j]->GetPath() == itemsForRemove[i])
      {
        m_vecItems->Remove(j);
        break;
      }
    }
  }
  m_vecItems->Append(itemsForAdd);

  CLog::Log(LOGDEBUG, "RetrieveMusicInfo() took %u msec",
            XbmcThreads::SystemClockMillis() - startTick);
}

/// \brief Add selected list/thumb control item to playlist and start playing
/// \param iItem Selected Item in list/thumb control
void CGUIWindowMusicBase::OnQueueItem(int iItem)
{
  // Determine the proper list to queue this element
  int playlist = CServiceBroker::GetPlaylistPlayer().GetCurrentPlaylist();
  if (playlist == PLAYLIST_NONE)
    playlist = g_application.GetAppPlayer().GetPreferredPlaylist();
  if (playlist == PLAYLIST_NONE)
    playlist = PLAYLIST_MUSIC;

  // don't re-queue items from playlist window
  if ( iItem < 0 || iItem >= m_vecItems->Size() || GetID() == WINDOW_MUSIC_PLAYLIST) return ;

  int iOldSize=CServiceBroker::GetPlaylistPlayer().GetPlaylist(playlist).size();

  // add item 2 playlist (make a copy as we alter the queuing state)
  CFileItemPtr item(new CFileItem(*m_vecItems->Get(iItem)));

  if (item->IsRAR() || item->IsZIP())
    return;

  //  Allow queuing of unqueueable items
  //  when we try to queue them directly
  if (!item->CanQueue())
    item->SetCanQueue(true);

  CLog::Log(LOGDEBUG, "Adding file %s%s to music playlist", item->GetPath().c_str(), item->m_bIsFolder ? " (folder) " : "");
  CFileItemList queuedItems;
  AddItemToPlayList(item, queuedItems);

  // select next item
  m_viewControl.SetSelectedItem(iItem + 1);

  // if party mode, add items but DONT start playing
  if (g_partyModeManager.IsEnabled())
  {
    g_partyModeManager.AddUserSongs(queuedItems, false);
    return;
  }

  CServiceBroker::GetPlaylistPlayer().Add(playlist, queuedItems);
  if (CServiceBroker::GetPlaylistPlayer().GetPlaylist(playlist).size() && !g_application.GetAppPlayer().IsPlaying())
  {
    if (m_guiState.get())
      m_guiState->SetPlaylistDirectory("playlistmusic://");

    CServiceBroker::GetPlaylistPlayer().Reset();
    CServiceBroker::GetPlaylistPlayer().SetCurrentPlaylist(playlist);
    CServiceBroker::GetPlaylistPlayer().Play(iOldSize, ""); // start playing at the first new item
  }
}

/// \brief Add unique file and folders and its subfolders to playlist
/// \param pItem The file item to add
void CGUIWindowMusicBase::AddItemToPlayList(const CFileItemPtr &pItem, CFileItemList &queuedItems)
{
  if (!pItem->CanQueue() || pItem->IsRAR() || pItem->IsZIP() || pItem->IsParentFolder()) // no zip/rar enqueues thank you!
    return;

  // fast lookup is needed here
  queuedItems.SetFastLookup(true);

  if (pItem->IsMusicDb() && pItem->m_bIsFolder && !pItem->IsParentFolder())
  { // we have a music database folder, just grab the "all" item underneath it
    CMusicDatabaseDirectory dir;
    if (!dir.ContainsSongs(pItem->GetPath()))
    { // grab the ALL item in this category
      // Genres will still require 2 lookups, and queuing the entire Genre folder
      // will require 3 lookups (genre, artist, album)
      CMusicDbUrl musicUrl;
      if (musicUrl.FromString(pItem->GetPath()))
      {
        musicUrl.AppendPath("-1/");
        CFileItemPtr item(new CFileItem(musicUrl.ToString(), true));
        item->SetCanQueue(true); // workaround for CanQueue() check above
        AddItemToPlayList(item, queuedItems);
      }
      return;
    }
  }
  if (pItem->m_bIsFolder)
  {
    // Check if we add a locked share
    if ( pItem->m_bIsShareOrDrive )
    {
      CFileItem item = *pItem;
      if ( !g_passwordManager.IsItemUnlocked( &item, "music" ) )
        return ;
    }

    // recursive
    CFileItemList items;
    GetDirectory(pItem->GetPath(), items);
    //OnRetrieveMusicInfo(items);
    FormatAndSort(items);
    for (int i = 0; i < items.Size(); ++i)
      AddItemToPlayList(items[i], queuedItems);
  }
  else
  {
    if (pItem->IsPlayList())
    {
      std::unique_ptr<CPlayList> pPlayList (CPlayListFactory::Create(*pItem));
      if (pPlayList.get())
      {
        // load it
        if (!pPlayList->Load(pItem->GetPath()))
        {
          HELPERS::ShowOKDialogText(CVariant{6}, CVariant{477});
          return; //hmmm unable to load playlist?
        }

        CPlayList playlist = *pPlayList;
        for (int i = 0; i < (int)playlist.size(); ++i)
        {
          AddItemToPlayList(playlist[i], queuedItems);
        }
        return;
      }
    }
    else if(pItem->IsInternetStream())
    { // just queue the internet stream, it will be expanded on play
      queuedItems.Add(pItem);
    }
    else if (pItem->IsPlugin() && pItem->GetProperty("isplayable") == "true")
    {
      // python files can be played
      queuedItems.Add(pItem);
    }
    else if (!pItem->IsNFO() && (pItem->IsAudio() || pItem->IsVideo()))
    {
      CFileItemPtr itemCheck = queuedItems.Get(pItem->GetPath());
      if (!itemCheck || itemCheck->m_lStartOffset != pItem->m_lStartOffset)
      { // add item
        CFileItemPtr item(new CFileItem(*pItem));
        m_musicdatabase.SetPropertiesForFileItem(*item);
        queuedItems.Add(item);
      }
    }
  }
}

void CGUIWindowMusicBase::UpdateButtons()
{
  CONTROL_ENABLE_ON_CONDITION(CONTROL_BTNRIP, g_mediaManager.IsAudio());

  CONTROL_ENABLE_ON_CONDITION(CONTROL_BTNSCAN,
                              !(m_vecItems->IsVirtualDirectoryRoot() ||
                                m_vecItems->IsMusicDb()));

  if (g_application.IsMusicScanning())
    SET_CONTROL_LABEL(CONTROL_BTNSCAN, 14056); // Stop Scan
  else
    SET_CONTROL_LABEL(CONTROL_BTNSCAN, 102); // Scan

  CGUIMediaWindow::UpdateButtons();
}

void CGUIWindowMusicBase::GetContextButtons(int itemNumber, CContextButtons &buttons)
{
  CFileItemPtr item;
  if (itemNumber >= 0 && itemNumber < m_vecItems->Size())
    item = m_vecItems->Get(itemNumber);

  if (item)
  {
    const CProfilesManager &profileManager = CServiceBroker::GetProfileManager();

    if (item && !item->IsParentFolder())
    {
      if (item->CanQueue() && !item->IsAddonsPath() && !item->IsScript())
      {
        buttons.Add(CONTEXT_BUTTON_QUEUE_ITEM, 13347); //queue

        // allow a folder to be ad-hoc queued and played by the default player
        if (item->m_bIsFolder || (item->IsPlayList() &&
           !g_advancedSettings.m_playlistAsFolders))
        {
          buttons.Add(CONTEXT_BUTTON_PLAY_ITEM, 208); // Play
        }
        else
        {
          const CPlayerCoreFactory &playerCoreFactory = CServiceBroker::GetPlayerCoreFactory();

          // check what players we have, if we have multiple display play with option
          std::vector<std::string> players;
          playerCoreFactory.GetPlayers(*item, players);
          if (players.size() >= 1)
            buttons.Add(CONTEXT_BUTTON_PLAY_WITH, 15213); // Play With...
        }
        if (item->IsSmartPlayList())
        {
            buttons.Add(CONTEXT_BUTTON_PLAY_PARTYMODE, 15216); // Play in Partymode
        }
        if (item->IsAudioBook())
        {
          int bookmark;
          if (m_musicdatabase.GetResumeBookmarkForAudioBook(item->GetPath(), bookmark) && bookmark > 0)
            buttons.Add(CONTEXT_BUTTON_RESUME_ITEM, 39016);
        }

        if (item->IsSmartPlayList() || m_vecItems->IsSmartPlayList())
          buttons.Add(CONTEXT_BUTTON_EDIT_SMART_PLAYLIST, 586);
        else if (item->IsPlayList() || m_vecItems->IsPlayList())
          buttons.Add(CONTEXT_BUTTON_EDIT, 586);
      }
      if (!m_vecItems->IsMusicDb() && !m_vecItems->IsInternetStream()           &&
          !item->IsPath("add") && !item->IsParentFolder() &&
          !item->IsPlugin() && !item->IsMusicDb()         &&
          !item->IsLibraryFolder() &&
          !StringUtils::StartsWithNoCase(item->GetPath(), "addons://")              &&
          (profileManager.GetCurrentProfile().canWriteDatabases() || g_passwordManager.bMasterUser))
      {
        buttons.Add(CONTEXT_BUTTON_SCAN, 13352);
      }
#ifdef HAS_DVD_DRIVE
      // enable Rip CD Audio or Track button if we have an audio disc
      if (g_mediaManager.IsDiscInDrive() && m_vecItems->IsCDDA())
      {
        // those cds can also include Audio Tracks: CDExtra and MixedMode!
        MEDIA_DETECT::CCdInfo *pCdInfo = g_mediaManager.GetCdInfo();
        if (pCdInfo->IsAudio(1) || pCdInfo->IsCDExtra(1) || pCdInfo->IsMixedMode(1))
          buttons.Add(CONTEXT_BUTTON_RIP_TRACK, 610);
      }
#endif
    }

    // enable CDDB lookup if the current dir is CDDA
    if (g_mediaManager.IsDiscInDrive() && m_vecItems->IsCDDA() &&
       (profileManager.GetCurrentProfile().canWriteDatabases() || g_passwordManager.bMasterUser))
    {
      buttons.Add(CONTEXT_BUTTON_CDDB, 16002);
    }
  }
  CGUIMediaWindow::GetContextButtons(itemNumber, buttons);
}

void CGUIWindowMusicBase::GetNonContextButtons(CContextButtons &buttons)
{
}

bool CGUIWindowMusicBase::OnContextButton(int itemNumber, CONTEXT_BUTTON button)
{
  CFileItemPtr item;
  if (itemNumber >= 0 && itemNumber < m_vecItems->Size())
    item = m_vecItems->Get(itemNumber);

  if (CGUIDialogContextMenu::OnContextButton("music", item, button))
  {
    if (button == CONTEXT_BUTTON_REMOVE_SOURCE)
      OnRemoveSource(itemNumber);

    Update(m_vecItems->GetPath());
    return true;
  }

  switch (button)
  {
  case CONTEXT_BUTTON_QUEUE_ITEM:
    OnQueueItem(itemNumber);
    return true;

  case CONTEXT_BUTTON_INFO:
    OnItemInfo(itemNumber);
    return true;

  case CONTEXT_BUTTON_EDIT:
    {
      std::string playlist = item->IsPlayList() ? item->GetPath() : m_vecItems->GetPath(); // save path as activatewindow will destroy our items
      g_windowManager.ActivateWindow(WINDOW_MUSIC_PLAYLIST_EDITOR, playlist);
      // need to update
      m_vecItems->RemoveDiscCache(GetID());
      return true;
    }

  case CONTEXT_BUTTON_EDIT_SMART_PLAYLIST:
    {
      std::string playlist = item->IsSmartPlayList() ? item->GetPath() : m_vecItems->GetPath(); // save path as activatewindow will destroy our items
      if (CGUIDialogSmartPlaylistEditor::EditPlaylist(playlist, "music"))
        Refresh(true); // need to update
      return true;
    }

  case CONTEXT_BUTTON_PLAY_ITEM:
    PlayItem(itemNumber);
    return true;

  case CONTEXT_BUTTON_PLAY_WITH:
    {
      const CPlayerCoreFactory &playerCoreFactory = CServiceBroker::GetPlayerCoreFactory();

      std::vector<std::string> players;
      playerCoreFactory.GetPlayers(*item, players);
      std::string player = playerCoreFactory.SelectPlayerDialog(players);
      if (!player.empty())
        OnClick(itemNumber, player);
      return true;
    }

  case CONTEXT_BUTTON_PLAY_PARTYMODE:
    g_partyModeManager.Enable(PARTYMODECONTEXT_MUSIC, item->GetPath());
    return true;

  case CONTEXT_BUTTON_RIP_CD:
    OnRipCD();
    return true;

#ifdef HAS_CDDA_RIPPER
  case CONTEXT_BUTTON_CANCEL_RIP_CD:
    CCDDARipper::GetInstance().CancelJobs();
    return true;
#endif

  case CONTEXT_BUTTON_RIP_TRACK:
    OnRipTrack(itemNumber);
    return true;

  case CONTEXT_BUTTON_SCAN:
    OnScan(itemNumber, true);
    return true;

  case CONTEXT_BUTTON_CDDB:
    if (m_musicdatabase.LookupCDDBInfo(true))
      Refresh();
    return true;

  case CONTEXT_BUTTON_RESUME_ITEM: //audiobooks
    {
      Update(item->GetPath());
      int bookmark;
      m_musicdatabase.GetResumeBookmarkForAudioBook(item->GetPath(), bookmark);
      int i=0;
      while (i < m_vecItems->Size() && bookmark > m_vecItems->Get(i)->m_lEndOffset)
        ++i;
      CFileItem resItem(*m_vecItems->Get(i));
      resItem.SetProperty("StartPercent", ((double)bookmark-resItem.m_lStartOffset)/(resItem.m_lEndOffset-resItem.m_lStartOffset)*100);
      g_application.PlayFile(resItem, "", false);
    }

  default:
    break;
  }

  return CGUIMediaWindow::OnContextButton(itemNumber, button);
}

bool CGUIWindowMusicBase::OnAddMediaSource()
{
  return CGUIDialogMediaSource::ShowAndAddMediaSource("music");
}

void CGUIWindowMusicBase::OnRipCD()
{
  if(g_mediaManager.IsAudio())
  {
    if (!g_application.CurrentFileItem().IsCDDA())
    {
#ifdef HAS_CDDA_RIPPER
      CCDDARipper::GetInstance().RipCD();
#endif
    }
    else
      HELPERS::ShowOKDialogText(CVariant{257}, CVariant{20099});
  }
}

void CGUIWindowMusicBase::OnRipTrack(int iItem)
{
  if(g_mediaManager.IsAudio())
  {
    if (!g_application.CurrentFileItem().IsCDDA())
    {
#ifdef HAS_CDDA_RIPPER
      CFileItemPtr item = m_vecItems->Get(iItem);
      CCDDARipper::GetInstance().RipTrack(item.get());
#endif
    }
    else
      HELPERS::ShowOKDialogText(CVariant{257}, CVariant{20099});
  }
}

void CGUIWindowMusicBase::PlayItem(int iItem)
{
  // restrictions should be placed in the appropriate window code
  // only call the base code if the item passes since this clears
  // the current playlist

  const CFileItemPtr pItem = m_vecItems->Get(iItem);
#ifdef HAS_DVD_DRIVE
  if (pItem->IsDVD())
  {
    MEDIA_DETECT::CAutorun::PlayDiscAskResume(pItem->GetPath());
    return;
  }
#endif

  // if its a folder, build a playlist
  if (pItem->m_bIsFolder && !pItem->IsPlugin())
  {
    // make a copy so that we can alter the queue state
    CFileItemPtr item(new CFileItem(*m_vecItems->Get(iItem)));

    //  Allow queuing of unqueueable items
    //  when we try to queue them directly
    if (!item->CanQueue())
      item->SetCanQueue(true);

    // skip ".."
    if (item->IsParentFolder())
      return;

    CFileItemList queuedItems;
    AddItemToPlayList(item, queuedItems);
    if (g_partyModeManager.IsEnabled())
    {
      g_partyModeManager.AddUserSongs(queuedItems, true);
      return;
    }

    /*
    std::string strPlayListDirectory = m_vecItems->GetPath();
    URIUtils::RemoveSlashAtEnd(strPlayListDirectory);
    */

    CServiceBroker::GetPlaylistPlayer().ClearPlaylist(PLAYLIST_MUSIC);
    CServiceBroker::GetPlaylistPlayer().Reset();
    CServiceBroker::GetPlaylistPlayer().Add(PLAYLIST_MUSIC, queuedItems);
    CServiceBroker::GetPlaylistPlayer().SetCurrentPlaylist(PLAYLIST_MUSIC);

    // play!
    CServiceBroker::GetPlaylistPlayer().Play();
  }
  else if (pItem->IsPlayList())
  {
    // load the playlist the old way
    LoadPlayList(pItem->GetPath());
  }
  else
  {
    // just a single item, play it
    //! @todo Add music-specific code for single playback of an item here (See OnClick in MediaWindow, and OnPlayMedia below)
    OnClick(iItem);
  }
}

void CGUIWindowMusicBase::LoadPlayList(const std::string& strPlayList)
{
  // if partymode is active, we disable it
  if (g_partyModeManager.IsEnabled())
    g_partyModeManager.Disable();

  // load a playlist like .m3u, .pls
  // first get correct factory to load playlist
  std::unique_ptr<CPlayList> pPlayList (CPlayListFactory::Create(strPlayList));
  if (pPlayList.get())
  {
    // load it
    if (!pPlayList->Load(strPlayList))
    {
      HELPERS::ShowOKDialogText(CVariant{6}, CVariant{477});
      return; //hmmm unable to load playlist?
    }
  }

  int iSize = pPlayList->size();
  if (g_application.ProcessAndStartPlaylist(strPlayList, *pPlayList, PLAYLIST_MUSIC))
  {
    if (m_guiState.get())
      m_guiState->SetPlaylistDirectory("playlistmusic://");
    // activate the playlist window if its not activated yet
    if (GetID() == g_windowManager.GetActiveWindow() && iSize > 1)
    {
      g_windowManager.ActivateWindow(WINDOW_MUSIC_PLAYLIST);
    }
  }
}

bool CGUIWindowMusicBase::OnPlayMedia(int iItem, const std::string &player)
{
  CFileItemPtr pItem = m_vecItems->Get(iItem);

  // party mode
  if (g_partyModeManager.IsEnabled())
  {
    CPlayList playlistTemp;
    playlistTemp.Add(pItem);
    g_partyModeManager.AddUserSongs(playlistTemp, true);
    return true;
  }
  else if (!pItem->IsPlayList() && !pItem->IsInternetStream())
  { // single music file - if we get here then we have autoplaynextitem turned off or queuebydefault
    // turned on, but we still want to use the playlist player in order to handle more queued items
    // following etc.
    if ( (CServiceBroker::GetSettings().GetBool(CSettings::SETTING_MUSICPLAYER_QUEUEBYDEFAULT) && g_windowManager.GetActiveWindow() != WINDOW_MUSIC_PLAYLIST_EDITOR) )
    {
      //! @todo Should the playlist be cleared if nothing is already playing?
      OnQueueItem(iItem);
      return true;
    }
    CServiceBroker::GetPlaylistPlayer().Play(pItem, player);
    return true;
  }
  return CGUIMediaWindow::OnPlayMedia(iItem, player);
}

void CGUIWindowMusicBase::UpdateThumb(const CAlbum &album, const std::string &path)
{
  const CProfilesManager &profileManager = CServiceBroker::GetProfileManager();

  // check user permissions
  bool saveDb = album.idAlbum != -1;
  bool saveDirThumb = true;
  if (!profileManager.GetCurrentProfile().canWriteDatabases() && !g_passwordManager.bMasterUser)
  {
    saveDb = false;
    saveDirThumb = false;
  }

  std::string albumThumb = m_musicdatabase.GetArtForItem(album.idAlbum, MediaTypeAlbum, "thumb");

  // Update the thumb in the music database (songs + albums)
  std::string albumPath(path);
  if (saveDb && CFile::Exists(albumThumb))
    m_musicdatabase.SaveAlbumThumb(album.idAlbum, albumThumb);

  // Update currently playing song if it's from the same album.  This is necessary as when the album
  // first gets it's cover, the info manager's item doesn't have the updated information (so will be
  // sending a blank thumb to the skin.)
  if (g_application.GetAppPlayer().IsPlayingAudio())
  {
    const CMusicInfoTag* tag=g_infoManager.GetCurrentSongTag();
    if (tag)
    {
      // really, this may not be enough as it is to reliably update this item.  eg think of various artists albums
      // that aren't tagged as such (and aren't yet scanned).  But we probably can't do anything better than this
      // in that case
      if (album.strAlbum == tag->GetAlbum() && (album.GetAlbumArtist() == tag->GetAlbumArtist() ||
                                                album.GetAlbumArtist() == tag->GetArtist()))
      {
        g_infoManager.SetCurrentAlbumThumb(albumThumb);
      }
    }
  }

  // Save this thumb as the directory thumb if it's the only album in the folder (files view nicety)
  // We do this by grabbing all the songs in the folder, and checking to see whether they come
  // from the same album.
  if (saveDirThumb && CFile::Exists(albumThumb) && !albumPath.empty() && !URIUtils::IsCDDA(albumPath))
  {
    CFileItemList items;
    GetDirectory(albumPath, items);
    OnRetrieveMusicInfo(items);
    VECALBUMS albums;
    CMusicInfoScanner::FileItemsToAlbums(items, albums);
    if (albums.size() == 1)
    { // set as folder thumb as well
      CMusicThumbLoader loader;
      loader.SetCachedImage(items, "thumb", albumPath);
    }
  }

  // update the file listing - we have to update the whole lot, as it's likely that
  // more than just our thumbnails changed
  //! @todo Ideally this would only be done when needed - at the moment we appear to be
  //!       doing this for every lookup, possibly twice (see ShowAlbumInfo)
  Refresh(true);

  //  Do we have to autoswitch to the thumb control?
  m_guiState.reset(CGUIViewState::GetViewState(GetID(), *m_vecItems));
  UpdateButtons();
}

void CGUIWindowMusicBase::OnRetrieveMusicInfo(CFileItemList& items)
{
  if (items.GetFolderCount()==items.Size() || items.IsMusicDb() ||
     (!CServiceBroker::GetSettings().GetBool(CSettings::SETTING_MUSICFILES_USETAGS) && !items.IsCDDA()))
  {
    return;
  }
  // Start the music info loader thread
  m_musicInfoLoader.SetProgressCallback(m_dlgProgress);
  m_musicInfoLoader.Load(items);

  bool bShowProgress=!g_windowManager.HasModalDialog();
  bool bProgressVisible=false;

  unsigned int tick=XbmcThreads::SystemClockMillis();

  while (m_musicInfoLoader.IsLoading())
  {
    if (bShowProgress)
    { // Do we have to init a progress dialog?
      unsigned int elapsed=XbmcThreads::SystemClockMillis()-tick;

      if (!bProgressVisible && elapsed>1500 && m_dlgProgress)
      { // tag loading takes more then 1.5 secs, show a progress dialog
        CURL url(items.GetPath());
        m_dlgProgress->SetHeading(CVariant{189});
        m_dlgProgress->SetLine(0, CVariant{505});
        m_dlgProgress->SetLine(1, CVariant{""});
        m_dlgProgress->SetLine(2, CVariant{url.GetWithoutUserDetails()});
        m_dlgProgress->Open();
        m_dlgProgress->ShowProgressBar(true);
        bProgressVisible = true;
      }

      if (bProgressVisible && m_dlgProgress && !m_dlgProgress->IsCanceled())
      { // keep GUI alive
        m_dlgProgress->Progress();
      }
    } // if (bShowProgress)
    Sleep(1);
  } // while (m_musicInfoLoader.IsLoading())

  if (bProgressVisible && m_dlgProgress)
    m_dlgProgress->Close();
}

bool CGUIWindowMusicBase::GetDirectory(const std::string &strDirectory, CFileItemList &items)
{
  items.ClearArt();
  bool bResult = CGUIMediaWindow::GetDirectory(strDirectory, items);
  if (bResult)
  {
    // We always want to expand disc images in music windows.
    CDirectory::FilterFileDirectories(items, ".iso", true);

    CMusicThumbLoader loader;
    loader.FillThumb(items);

    CQueryParams params;
    CDirectoryNode::GetDatabaseInfo(items.GetPath(), params);

    // Get art for directory when album or artist
    bool artfound = false;
    std::vector<ArtForThumbLoader> art;
    if (params.GetAlbumId() > 0)
    { // Get album and related artist(s) art
      artfound = m_musicdatabase.GetArtForItem(-1, params.GetAlbumId(), -1, false, art);
    }
    else if (params.GetArtistId() > 0)
    { // get artist art
      artfound = m_musicdatabase.GetArtForItem(-1, -1, params.GetArtistId(), true, art);
    }
    if (artfound)
    {
      std::string dirType = MediaTypeArtist;
      if (params.GetAlbumId() > 0)
        dirType = MediaTypeAlbum;
      std::map<std::string, std::string> artmap;
      for (auto artitem : art)
      {
        std::string artname;
        if (dirType == artitem.mediaType)
          artname = artitem.artType;
        else if (artitem.prefix.empty())
          artname = artitem.mediaType + "." + artitem.artType;
        else
        {
          if (dirType == MediaTypeAlbum)
            StringUtils::Replace(artitem.prefix, "albumartist", "artist");
          artname = artitem.prefix + "." + artitem.artType;
        }
      }
      items.SetArt(artmap);
    }

    // add in the "New Playlist" item if we're in the playlists folder
    if ((items.GetPath() == "special://musicplaylists/") && !items.Contains("newplaylist://"))
    {
      const CProfilesManager &profileManager = CServiceBroker::GetProfileManager();

      CFileItemPtr newPlaylist(new CFileItem(profileManager.GetUserDataItem("PartyMode.xsp"),false));
      newPlaylist->SetLabel(g_localizeStrings.Get(16035));
      newPlaylist->SetLabelPreformatted(true);
      newPlaylist->SetIconImage("DefaultPartyMode.png");
      newPlaylist->m_bIsFolder = true;
      items.Add(newPlaylist);

      newPlaylist.reset(new CFileItem("newplaylist://", false));
      newPlaylist->SetLabel(g_localizeStrings.Get(525));
      newPlaylist->SetIconImage("DefaultAddSource.png");
      newPlaylist->SetLabelPreformatted(true);
      newPlaylist->SetSpecialSort(SortSpecialOnBottom);
      newPlaylist->SetCanQueue(false);
      items.Add(newPlaylist);

      newPlaylist.reset(new CFileItem("newsmartplaylist://music", false));
      newPlaylist->SetLabel(g_localizeStrings.Get(21437));
      newPlaylist->SetIconImage("DefaultAddSource.png");
      newPlaylist->SetLabelPreformatted(true);
      newPlaylist->SetSpecialSort(SortSpecialOnBottom);
      newPlaylist->SetCanQueue(false);
      items.Add(newPlaylist);
    }

    // check for .CUE files here.
    items.FilterCueItems();

    std::string label;
    if (items.GetLabel().empty() && m_rootDir.IsSource(items.GetPath(), CMediaSourceSettings::GetInstance().GetSources("music"), &label))
      items.SetLabel(label);
  }

  return bResult;
}

bool CGUIWindowMusicBase::CheckFilterAdvanced(CFileItemList &items) const
{
  std::string content = items.GetContent();
  if ((items.IsMusicDb() || CanContainFilter(m_strFilterPath)) &&
      (StringUtils::EqualsNoCase(content, "artists") ||
       StringUtils::EqualsNoCase(content, "albums")  ||
       StringUtils::EqualsNoCase(content, "songs")))
    return true;

  return false;
}

bool CGUIWindowMusicBase::CanContainFilter(const std::string &strDirectory) const
{
  return URIUtils::IsProtocol(strDirectory, "musicdb");
}

void CGUIWindowMusicBase::OnInitWindow()
{
  CGUIMediaWindow::OnInitWindow();
  // Prompt for rescan of library to read music file tags that were not processed by previous versions
  // and accomodate any changes to the way some tags are processed
  if (m_musicdatabase.GetMusicNeedsTagScan() != 0)
  {
    if (g_infoManager.GetLibraryBool(LIBRARY_HAS_MUSIC) && !g_application.IsMusicScanning())
    {
      // rescan of music library required
      if (CGUIDialogYesNo::ShowAndGetInput(CVariant{799}, CVariant{38060}))
      {
        int flags = CMusicInfoScanner::SCAN_RESCAN;
        // When set to fetch information on update enquire about scraping that as well
        // It may take some time, so the user may want to do it later by "Query Info For All"
        if (CServiceBroker::GetSettings().GetBool(CSettings::SETTING_MUSICLIBRARY_DOWNLOADINFO))
          if (CGUIDialogYesNo::ShowAndGetInput(CVariant{799}, CVariant{38061}))
            flags |= CMusicInfoScanner::SCAN_ONLINE;
        g_application.StartMusicScan("", true, flags);
        m_musicdatabase.SetMusicTagScanVersion(); // once is enough (user may interrupt, but that's up to them)
      }
    }
    else
    {
      // no need to force a rescan if there's no music in the library or if a library scan is already active
      m_musicdatabase.SetMusicTagScanVersion();
    }
  }
}

std::string CGUIWindowMusicBase::GetStartFolder(const std::string &dir)
{
  std::string lower(dir); StringUtils::ToLower(lower);
  if (lower == "plugins" || lower == "addons")
    return "addons://sources/audio/";
  else if (lower == "$playlists" || lower == "playlists")
    return "special://musicplaylists/";
  return CGUIMediaWindow::GetStartFolder(dir);
}

void CGUIWindowMusicBase::OnScan(int iItem, bool bPromptRescan /*= false*/)
{
  std::string strPath;
  if (iItem < 0 || iItem >= m_vecItems->Size())
    strPath = m_vecItems->GetPath();
  else if (m_vecItems->Get(iItem)->m_bIsFolder)
    strPath = m_vecItems->Get(iItem)->GetPath();
  else
  { //! @todo MUSICDB - should we allow scanning a single item into the database?
    //!       This will require changes to the info scanner, which assumes we're running on a folder
    strPath = m_vecItems->GetPath();
  }
  // Ask for full rescan of music files when scan item from file view context menu
  bool doRescan = false;
  if (bPromptRescan)
    doRescan = CGUIDialogYesNo::ShowAndGetInput(CVariant{ 799 }, CVariant{ 38062 });

  DoScan(strPath, doRescan);
}

void CGUIWindowMusicBase::DoScan(const std::string &strPath, bool bRescan /*= false*/)
{
  if (g_application.IsMusicScanning())
  {
    g_application.StopMusicScan();
    return;
  }

  // Start background loader
  int iControl=GetFocusedControlID();
  int flags = 0;
  if (bRescan)
    flags = CMusicInfoScanner::SCAN_RESCAN;
  if (CServiceBroker::GetSettings().GetBool(CSettings::SETTING_MUSICLIBRARY_DOWNLOADINFO))
    flags |= CMusicInfoScanner::SCAN_ONLINE;
  g_application.StartMusicScan(strPath, true, flags);
  SET_CONTROL_FOCUS(iControl, 0);
  UpdateButtons();
}

void CGUIWindowMusicBase::OnRemoveSource(int iItem)
{
  bool bCanceled;
  if (CGUIDialogYesNo::ShowAndGetInput(CVariant{522}, CVariant{20340}, bCanceled, CVariant{""}, CVariant{""}, CGUIDialogYesNo::NO_TIMEOUT))
  {
    MAPSONGS songs;
    CMusicDatabase database;
    database.Open();
    database.RemoveSongsFromPath(m_vecItems->Get(iItem)->GetPath(), songs, false);
    database.CleanupOrphanedItems();
    g_infoManager.ResetLibraryBools();
    m_vecItems->RemoveDiscCache(GetID());
  }
}

void CGUIWindowMusicBase::OnPrepareFileItems(CFileItemList &items)
{
  CGUIMediaWindow::OnPrepareFileItems(items);

  if (!items.IsMusicDb() && !items.IsSmartPlayList())
    RetrieveMusicInfo();
}

void CGUIWindowMusicBase::OnAssignContent(const std::string &path)
{
  // Music scrapers are not source specific, so unlike video there is no content selection logic here.
  // Called on having added a music source, this starts scanning items into library when required
 
  // "Add to library" yes/no dialog with additional "settings" custom button
  // "Do you want to add the media from this source to your library?"
  DialogResponse rep = DialogResponse::CUSTOM;
  while (rep == DialogResponse::CUSTOM)
  {
    rep = HELPERS::ShowYesNoCustomDialog(CVariant{20444}, CVariant{20447}, CVariant{106}, CVariant{107}, CVariant{10004});
    if (rep == DialogResponse::CUSTOM)
      // Edit default info provider settings so can be applied during scan
      CGUIDialogInfoProviderSettings::Show();
  }  
  if (rep == DialogResponse::YES)  
    g_application.StartMusicScan(path, true);
  
}

