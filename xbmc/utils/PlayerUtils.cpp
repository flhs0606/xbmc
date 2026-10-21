/*
 *  Copyright (C) 2022 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PlayerUtils.h"

#include "FileItem.h"
#include "application/ApplicationPlayer.h"
#include "music/MusicUtils.h"
#include "utils/Variant.h"
#include "video/VideoUtils.h"

bool CPlayerUtils::IsItemPlayable(const CFileItem& itemIn)
{
  const CFileItem item(itemIn.GetItemToPlay());

  // General
  if (item.IsParentFolder())
    return false;

  // Plugins
  if (item.IsPlugin() && item.GetProperty("isplayable").asBoolean())
    return true;

  // Music
  if (MUSIC_UTILS::IsItemPlayable(item))
    return true;

  // Movies / TV Shows / Music Videos
  if (VIDEO_UTILS::IsItemPlayable(item))
    return true;

  //! @todo add more types on demand.

  return false;
}

void CPlayerUtils::AdvanceTempoStep(std::shared_ptr<CApplicationPlayer> appPlayer,
                                    TempoStepChange change)
{
  static const std::vector<float> tempoSteps = {1.0f, 1.25f, 1.5f, 2.0f};
  const float currentTempo = appPlayer->GetPlayTempo();

  if (change == TempoStepChange::INCREASE)
  {
    for (float step : tempoSteps)
    {
      if (step > currentTempo + 0.01f)
      {
        appPlayer->SetTempo(step);
        return;
      }
    }
    // Loop back to 1.0x if already at or above maximum step
    appPlayer->SetTempo(1.0f);
  }
  else if (change == TempoStepChange::DECREASE)
  {
    for (auto it = tempoSteps.rbegin(); it != tempoSteps.rend(); ++it)
    {
      if (*it < currentTempo - 0.01f)
      {
        appPlayer->SetTempo(*it);
        return;
      }
    }
    // Loop back to highest step if already at or below 1.0x
    appPlayer->SetTempo(tempoSteps.back());
  }
}
