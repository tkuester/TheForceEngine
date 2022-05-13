#include "imuse.h"
#include "imDigitalSound.h"
#include "imTrigger.h"
#include "imList.h"
#include <TFE_Jedi/Math/core_math.h>
#include <TFE_System/system.h>
#include <TFE_Audio/midi.h>
#include <assert.h>

namespace TFE_Jedi
{
	////////////////////////////////////////////////////
	// Structures
	////////////////////////////////////////////////////
	struct ImWaveSound
	{
		s32 u00;
		ImWaveSound* next;
		u32 waveStreamFlag;
		ImSoundId soundId;
		s32 marker;
		s32 group;
		s32 priority;
		s32 baseVolume;
		s32 volume;
		s32 pan;
		s32 detune;
		s32 transpose;
		s32 detuneTrans;
		s32 mailbox;
	};

	struct ImWaveData;

	struct ImWavePlayer
	{
		ImWavePlayer* prev;
		ImWavePlayer* next;
		ImWaveData* data;
		ImSoundId soundId;

		s32 u10;
		s32 u14;
		s32 priority;
		s32 volume;

		s32 baseVol;
		s32 pan;
		s32 u28;
		s32 u2c;

		s32 u30;
		s32 u34;
		s32 u38;
	};

	struct ImWaveData
	{
		ImWavePlayer* player;
		s32 offset;
		s32 chunkSize;
		s32 baseOffset;

		s32 chunkIndex;
		s32 u14;
		s32 u18;
		s32 u1c;

		s32 u20;
		s32 u24;
		s32 u28;
		s32 u2c;

		s32 u30;
	};
	
	/////////////////////////////////////////////////////
	// Internal State
	/////////////////////////////////////////////////////
	static ImWaveSound* s_imWaveSounds = nullptr;
	static ImWaveSound  s_imWaveSoundStore[16];
	static ImWavePlayer s_imWavePlayer[16];
	static ImWaveData   s_imWaveData[16];
	static u8  s_imWaveChunkData[48];
	static s32 s_imWaveMixCount = 8;
	static s32 s_imWaveNanosecsPerSample;
	static iMuseInitData* s_imDigitalData;

	static u8  s_imWaveFalloffTableMem[2052];
	static u8* s_imWaveFalloffTable = &s_imWaveFalloffTableMem[1028];

	extern atomic_s32 s_sndPlayerLock;
	extern atomic_s32 s_digitalPause;

	extern void ImMidiPlayerLock();
	extern void ImMidiPlayerUnlock();
	extern s32 ImWrapValue(s32 value, s32 a, s32 b);
	extern s32 ImGetGroupVolume(s32 group);
	extern u8* ImInternalGetSoundData(ImSoundId soundId);

	ImWaveData* ImGetWaveData(s32 index);
	s32 ImComputeDigitalFalloff(iMuseInitData* initData);
	s32 ImSetWaveParamInternal(ImSoundId soundId, s32 param, s32 value);
	s32 ImGetWaveParamIntern(ImSoundId soundId, s32 param);
	s32 ImStartDigitalSoundIntern(ImSoundId soundId, s32 priority, s32 chunkIndex);
	
	/////////////////////////////////////////////////////////// 
	// API
	/////////////////////////////////////////////////////////// 
	s32 ImInitializeDigitalAudio(iMuseInitData* initData)
	{
		IM_DBG_MSG("TRACKS module...");
		if (initData->waveMixCount <= 0 || initData->waveMixCount > 16)
		{
			IM_LOG_ERR("TR: waveMixCount NULL or too big, defaulting to 4...");
			initData->waveMixCount = 4;
		}
		s_imWaveMixCount = initData->waveMixCount;
		s_digitalPause = 0;
		s_imWaveSounds = nullptr;

		if (initData->waveSpeed == IM_WAVE_11kHz) // <- this is the path taken by Dark Forces DOS
		{
			// Nanoseconds per second / wave speed in Hz
			// 1,000,000,000 / 11,000
			s_imWaveNanosecsPerSample = 90909;
		}
		else // IM_WAVE_22kHz
		{
			// Nanoseconds per second / wave speed in Hz
			// 1,000,000,000 / 22,000
			s_imWaveNanosecsPerSample = 45454;
		}

		ImWavePlayer* player = s_imWavePlayer;
		for (s32 i = 0; i < s_imWaveMixCount; i++, player++)
		{
			player->prev = nullptr;
			player->next = nullptr;
			ImWaveData* data = ImGetWaveData(i);
			player->data = data;
			data->player = player;
			player->soundId = IM_NULL_SOUNDID;
		}

		s_sndPlayerLock = 0;
		return ImComputeDigitalFalloff(initData);
	}

	s32 ImSetWaveParam(ImSoundId soundId, s32 param, s32 value)
	{
		ImMidiPlayerLock();
		s32 res = ImSetWaveParamInternal(soundId, param, value);
		ImMidiPlayerUnlock();
		return res;
	}

	s32 ImGetWaveParam(ImSoundId soundId, s32 param)
	{
		ImMidiPlayerLock();
		s32 res = ImGetWaveParamIntern(soundId, param);
		ImMidiPlayerUnlock();
		return res;
	}

	s32 ImStartDigitalSound(ImSoundId soundId, s32 priority)
	{
		ImMidiPlayerLock();
		s32 res = ImStartDigitalSoundIntern(soundId, priority, 0);
		ImMidiPlayerUnlock();
		return res;
	}

	////////////////////////////////////
	// Internal
	////////////////////////////////////
	ImWaveData* ImGetWaveData(s32 index)
	{
		return &s_imWaveData[index];
	}

	s32 ImComputeDigitalFalloff(iMuseInitData* initData)
	{
		s_imDigitalData = initData;
		s32 waveMixCount = initData->waveMixCount;
		s32 volumeMidPoint = 128;
		s32 tableSize = waveMixCount << 7;
		for (s32 i = 0; i < tableSize; i++)
		{
			// Results for count ~= 8: (i=0) 0.0, 1.5, 2.5, 3.4, 4.4, 5.2, 6.3, 7.2, ... 127.1 (i = 1023).
			s32 volumeOffset = ((waveMixCount * 127 * i) << 8) / (waveMixCount * 127 + (waveMixCount - 1)*i) + 128;
			volumeOffset >>= 8;

			s_imWaveFalloffTable[i] = volumeMidPoint + volumeOffset;
			u8* vol = s_imWaveFalloffTable - i - 1;
			*vol = volumeMidPoint - volumeOffset - 1;
		}
		return imSuccess;
	}

	s32 ImSetWaveParamInternal(ImSoundId soundId, s32 param, s32 value)
	{
		ImWaveSound* sound = s_imWaveSounds;
		while (sound)
		{
			if (sound->soundId == soundId)
			{
				if (param == soundGroup)
				{
					if (value >= 16)
					{
						return imArgErr;
					}
					sound->volume = ((sound->baseVolume + 1) * ImGetGroupVolume(value)) >> 7;
					return imSuccess;
				}
				else if (param == soundPriority)
				{
					if (value > 127)
					{
						return imArgErr;
					}
					sound->priority = value;
					return imSuccess;
				}
				else if (param == soundVol)
				{
					if (value > 127)
					{
						return imArgErr;
					}
					sound->baseVolume = value;
					sound->volume = ((sound->baseVolume + 1) * ImGetGroupVolume(sound->group)) >> 7;
					return imSuccess;
				}
				else if (param == soundPan)
				{
					if (value > 127)
					{
						return imArgErr;
					}
					sound->pan = value;
					return imSuccess;
				}
				else if (param == soundDetune)
				{
					if (value < -9216 || value > 9216)
					{
						return imArgErr;
					}
					sound->detune = value;
					sound->detuneTrans = sound->detune + (sound->transpose << 8);
					return imSuccess;
				}
				else if (param == soundTranspose)
				{
					if (value < -12 || value > 12)
					{
						return imArgErr;
					}
					sound->transpose = value ? ImWrapValue(sound->transpose + value, -12, 12) : 0;
					sound->detuneTrans = sound->detune + (sound->transpose << 8);
					return imSuccess;
				}
				else if (param == soundMailbox)
				{
					sound->mailbox = value;
					return imSuccess;
				}
				// Invalid Parameter
				IM_LOG_ERR("ERR: TrSetParam() couldn't set param %lu...", param);
				return imArgErr;
			}
			sound = sound->next;
		}
		return imInvalidSound;
	}

	s32 ImGetWaveParamIntern(ImSoundId soundId, s32 param)
	{
		s32 soundCount = 0;
		ImWaveSound* sound = s_imWaveSounds;
		while (sound)
		{
			if (sound->soundId == soundId)
			{
				if (param == soundType)
				{
					return imFail;
				}
				else if (param == soundPlayCount)
				{
					soundCount++;
				}
				else if (param == soundMarker)
				{
					return sound->marker;
				}
				else if (param == soundGroup)
				{
					return sound->group;
				}
				else if (param == soundPriority)
				{
					return sound->priority;
				}
				else if (param == soundVol)
				{
					return sound->baseVolume;
				}
				else if (param == soundPan)
				{
					return sound->pan;
				}
				else if (param == soundDetune)
				{
					return sound->detune;
				}
				else if (param == soundTranspose)
				{
					return sound->transpose;
				}
				else if (param == soundMailbox)
				{
					return sound->mailbox;
				}
				else if (param == waveStreamFlag)
				{
					return sound->waveStreamFlag ? 1 : 0;
				}
				else
				{
					return imArgErr;
				}
			}
			sound = sound->next;
		}
		return (param == soundPlayCount) ? soundCount : imInvalidSound;
	}

	ImWavePlayer* ImAllocWavePlayer(s32 priority)
	{
		ImWavePlayer* player = s_imWavePlayer;
		ImWavePlayer* newPlayer = nullptr;
		for (s32 i = 0; i < s_imWaveMixCount; i++, player++)
		{
			if (!player->soundId)
			{
				return player;
			}
		}

		IM_LOG_WRN("ERR: no spare tracks...");
		// TODO
		return nullptr;
	}

	u8* ImGetChunkSoundData(s32 chunkIndex, s32 rangeMin, s32 rangeMax)
	{
		IM_LOG_ERR("Digital Sound chunk index should be zero in Dark Forces, but is %d.", chunkIndex);
		assert(0);
		return nullptr;
	}

	s32 ImSeekToNextChunk(ImWaveData* data)
	{
		while (1)
		{
			u8* chunkData = s_imWaveChunkData;
			u8* sndData = nullptr;

			if (data->chunkIndex)
			{
				sndData = ImGetChunkSoundData(data->chunkIndex, 0, 48);
				if (!sndData)
				{
					sndData = ImGetChunkSoundData(data->chunkIndex, 0, 1);
				}
				if (!sndData)
				{
					return imNotFound;
				}
			}
			else  // chunkIndex == 0
			{
				ImWavePlayer* player = data->player;
				sndData = ImInternalGetSoundData(player->soundId);
				if (!sndData)
				{
					if (player->u34 == 0)
					{
						player->u34 = 8;
					}
					IM_LOG_ERR("null sound addr in SeekToNextChunk()...");
					return imFail;
				}
			}

			memcpy(chunkData, sndData + data->offset, 48);
			u8 id = *chunkData;
			chunkData++;

			if (id == 0)
			{
				return imFail;
			}
			else if (id == 1)	// found the next useful chunk.
			{
				s32 chunkSize = (chunkData[0] | (chunkData[1] << 8) | (chunkData[2] << 16)) - 2;
				chunkData += 5;

				data->chunkSize = chunkSize;
				if (chunkSize > 220000)
				{
					ImWavePlayer* player = data->player;
					if (player->u34 == 0)
					{
						player->u34 = 9;
					}
				}

				data->offset += 6;
				if (data->chunkIndex)
				{
					IM_LOG_ERR("data->chunkIndex should be 0 in Dark Forces, it is: %d.", data->chunkIndex);
					assert(0);
				}
				return imSuccess;
			}
			else if (id == 4)
			{
				chunkData += 3;
				ImSetSoundTrigger((ImSoundId)data->player, chunkData);
				data->offset += 6;
			}
			else if (id == 6)
			{
				data->baseOffset = data->offset;
				data->offset += 6;
				if (data->chunkIndex != 0)
				{
					IM_LOG_ERR("data->chunkIndex should be 0 in Dark Forces, it is: %d.", data->chunkIndex);
					assert(0);
				}
			}
			else if (id == 7)
			{
				data->offset = data->baseOffset;
				if (data->chunkIndex != 0)
				{
					IM_LOG_ERR("data->chunkIndex should be 0 in Dark Forces, it is: %d.", data->chunkIndex);
					assert(0);
				}
			}
			else if (id == 'C')
			{
				if (chunkData[0] != 'r' || chunkData[1] != 'e' || chunkData[2] != 'a')
				{
					IM_LOG_ERR("ERR: Illegal chunk in sound %lu...", data->player->soundId);
					return imFail;
				}
				data->offset += 26;
				if (data->chunkIndex)
				{
					IM_LOG_ERR("data->chunkIndex should be 0 in Dark Forces, it is: %d.", data->chunkIndex);
					assert(0);
				}
			}
			else
			{
				IM_LOG_ERR("ERR: Illegal chunk in sound %lu...", data->player->soundId);
				return imFail;
			}
		}
		return imSuccess;
	}

	s32 ImWaveSetupPlayerData(ImWavePlayer* player, s32 chunkIndex)
	{
		ImWaveData* data = player->data;
		data->offset = 0;
		data->chunkSize = 0;
		data->baseOffset = 0;
		data->u20 = 0;

		if (chunkIndex)
		{
			IM_LOG_ERR("data->chunkIndex should be 0 in Dark Forces, it is: %d.", chunkIndex);
			assert(0);
		}

		data->chunkIndex = 0;
		return ImSeekToNextChunk(data);
	}

	s32 ImStartDigitalSoundIntern(ImSoundId soundId, s32 priority, s32 chunkIndex)
	{
		priority = clamp(priority, 0, 127);
		ImWavePlayer* player = ImAllocWavePlayer(priority);
		if (!player)
		{
			return imFail;
		}

		player->soundId = soundId;
		player->u10 = 0;
		player->u14 = 0;
		player->priority = priority;
		player->volume = 128;
		player->baseVol = ImGetGroupVolume(0);
		player->pan = 64;
		player->u28 = 0;
		player->u2c = 0;
		player->u30 = 0;
		player->u34 = 0;
		player->u38 = 0;
		if (ImWaveSetupPlayerData(player, chunkIndex) != imSuccess)
		{
			IM_LOG_ERR("Failed to setup wave player data - soundId: 0x%x, priority: %d", soundId, priority);
			return imFail;
		}

		ImMidiPlayerLock();
		IM_LIST_ADD(s_imWaveSounds, player);
		ImMidiPlayerUnlock();

		return imSuccess;
	}

}  // namespace TFE_Jedi
