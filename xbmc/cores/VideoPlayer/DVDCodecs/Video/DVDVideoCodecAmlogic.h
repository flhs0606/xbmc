/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDVideoCodec.h"
#include "DVDStreamInfo.h"
#include "settings/lib/ISettingCallback.h"
#include "threads/CriticalSection.h"
#include "cores/VideoPlayer/Buffers/VideoBuffer.h"
#include "utils/BitstreamConverter.h"
#include "utils/VC1BitstreamParser.h"
#include "AMLFrameMetadata.h"

#include <cstddef>
#include <atomic>
#include <memory>
#include <set>
#include <vector>

class CAMLCodec;
struct mpeg2_sequence;
struct h264_sequence;
class CBitstreamParser;
class CBitstreamConverter;
class CDataCacheCore;
class CSetting;

class CDVDVideoCodecAmlogic;

struct DLDemuxPacket
{
  FFmpegExtraData buffer;
  uint32_t size{0};
  bool isELPackage{false};
  double dts{0.0};
  double pts{0.0};
};

class CAMLVideoBuffer : public CVideoBuffer
{
public:
  CAMLVideoBuffer(int id) : CVideoBuffer(id) {};
  void Set(CDVDVideoCodecAmlogic *codec, std::shared_ptr<CAMLCodec> amlcodec, uint64_t omxPts, int amlDuration, uint32_t bufferIndex)
  {
    m_codec = codec;
    m_amlCodec = amlcodec;
    m_omxPts = omxPts;
    m_amlDuration = amlDuration;
    m_bufferIndex = bufferIndex;
  }

  CDVDVideoCodecAmlogic* m_codec;
  std::shared_ptr<CAMLCodec> m_amlCodec;
  uint64_t m_omxPts;
  int m_amlDuration;
  uint32_t m_bufferIndex;
};

class CAMLVideoBufferPool : public IVideoBufferPool
{
public:
  virtual ~CAMLVideoBufferPool();

  virtual CVideoBuffer* Get() override;
  virtual void Return(int id) override;
  void ReleaseAllBuffers();

private:
  CCriticalSection m_criticalSection;;
  std::vector<CAMLVideoBuffer*> m_videoBuffers;
  std::vector<int> m_freeBuffers;
};

class CDVDVideoCodecAmlogic : public CDVDVideoCodec, public ISettingCallback
{
public:
  CDVDVideoCodecAmlogic(CProcessInfo &processInfo);
  virtual ~CDVDVideoCodecAmlogic();

  static std::unique_ptr<CDVDVideoCodec> Create(CProcessInfo& processInfo);
  static bool Register();

  // Required overrides
  virtual bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options) override;
  virtual bool AddData(const DemuxPacket &packet) override;
  virtual void Reset() override;
  virtual VCReturn GetPicture(VideoPicture* pVideoPicture) override;
  virtual void SetSpeed(int iSpeed) override;
  virtual void SetCodecControl(int flags) override;
  virtual void Abort() override;
  virtual const char* GetName(void) override { return (const char*)m_pFormatName; }
  virtual bool SupportsExtention() { return true; }
  bool HonorsAccurateSeek() const override { return false; }

  void OnSettingChanged(const std::shared_ptr<const CSetting>& setting) override;

protected:
  void Close(void);
  void FrameRateTracking(uint8_t *pData, int iSize, double dts, double pts);

  std::shared_ptr<CAMLCodec> m_Codec;

  const char     *m_pFormatName;
  VideoPicture    m_videobuffer;
  bool            m_opened;
  int             m_codecControlFlags;
  CDVDStreamInfo  m_hints;
  double          m_framerate;
  int             m_video_rate;
  float           m_aspect_ratio;
  double          m_mpeg2_sequence_pts;
  double          m_h264_sequence_pts;
  bool            m_has_keyframe;
  bool m_hdr10PlusUpgraded{false};

  std::unique_ptr<mpeg2_sequence> m_mpeg2_sequence;
  std::unique_ptr<h264_sequence>  m_h264_sequence;

  std::unique_ptr<CBitstreamParser>    m_bitparser;
  std::unique_ptr<CBitstreamConverter> m_bitstream;
  std::unique_ptr<CVC1BitstreamParser> m_vc1Parser;
  bool m_vc1ScanDetected{false};
  bool m_vc1ForceFrameIntEnabled{false};
  bool m_vc1DropFrameEnabled{false};
  int  m_vc1ForceFrameIntLast{-1};
  bool m_vc1ForceFrameIntLocked{false};
private:
  static constexpr std::size_t MAX_CACHED_DUAL_LAYER_PACKETS = 2;

  bool DualLayerConvert(uint8_t *pData, uint32_t iSize, const DemuxPacket &packet);
  bool SingleLayerConvert(uint8_t *pData, uint32_t iSize, const DemuxPacket &packet) const;
  DLDemuxPacket AcquireDualLayerPacket(std::size_t requiredCapacity);
  void RecycleDualLayerPacket(DLDemuxPacket&& packet);
  void ClearBitstreamCommon(void);
  void UpdateAppendCMv40SettingCache();
  void ApplyDynamicDoViSettings();
  DOVICMv40Mode EffectiveCMv40Mode(DOVICMv40Mode mode);

  std::shared_ptr<CAMLVideoBufferPool> m_videoBufferPool;
  static std::atomic<bool> m_InstanceGuard;

  std::list<DLDemuxPacket> m_packages;
  std::vector<DLDemuxPacket> m_freePackages;

  uint32_t m_dlStatBL = 0;
  uint32_t m_dlStatEL = 0;
  uint32_t m_dlStatPaired = 0;
  uint32_t m_dlStatEvicted = 0;
  double m_dlStatMissDelta = 0.0;
  double m_dlLastPts{DVD_NOPTS_VALUE};
  double m_dlLastDts{DVD_NOPTS_VALUE};
  int64_t m_dlStatLastLog = 0;

  bool      m_last_added = true;
  uint8_t  *m_last_pData = nullptr;
  uint32_t  m_last_iSize = 0;

  std::atomic<int> m_appendCMv40ModeSetting{static_cast<int>(DOVICMv40Mode::CMV40_NONE)};
  DOVICMv40Mode m_appendCMv40ModeApplied{DOVICMv40Mode::CMV40_NONE};
  std::atomic<int> m_appendCMv40AutoThresholdSetting{static_cast<int>(DOVICMv40AutoThreshold::CMV40_AUTO_SOURCE)};
  DOVICMv40AutoThreshold m_appendCMv40AutoThresholdApplied{DOVICMv40AutoThreshold::CMV40_AUTO_SOURCE};
  std::atomic<int> m_appendCMv40Auto2ThresholdSetting{20};
  int m_appendCMv40Auto2ThresholdApplied{20};
  std::atomic<int> m_cmv40DisplayMaxLumSetting{0};
  int m_cmv40DisplayMaxLumApplied{-1};
  std::atomic<int> m_cmv40DvTypeSetting{0};
  int m_cmv40DvTypeApplied{-1};
  std::atomic<int> m_cmv40VideoProcessorSetting{0};
  int m_cmv40VideoProcessorApplied{-1};
  bool m_cmv40Vs10ConvertingApplied{false};
  std::atomic<unsigned int> m_dvSettingsGeneration{0};
  unsigned int m_dvSettingsGenApplied{0};
  bool m_cmv40Auto2PinnedLogged{false};
  bool m_cmv40VideoProcessorLogged{false};
  bool m_settingsCallbackRegistered{false};

  void DrainMetadataToClock();
  double RenderDisplayLatency();

  uint32_t m_metadataToken{0};
  bool m_metaLeadLogged{false};
  bool m_dualLayer{false};
  bool m_stripHdr10Plus{false};
  int m_nalLengthSize{0};
  double m_lastCommitPts{0.0};
  AMLFrameMetadata m_streamMeta;
  AMLFrameMetadata m_pendingMeta;
  AMLFrameMetadata m_lastMeta;
  CAMLFrameMetadataSequencer m_metadataSequencer;

};
