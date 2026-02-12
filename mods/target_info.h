/**
 * @file target_info.h
 * @brief TargetInfo mod — enhanced target window with distance, LoS, and PH info.
 * @date 2026-02-12
 *
 * Port of MQ2TargetInfo by EqMule. Adds overlays to the target window showing
 * target level/race/class, distance, line-of-sight, and placeholder status.
 */

#pragma once

#include "mod_interface.h"

class TargetInfoMod : public IMod
{
public:
    const char* GetName() const override { return "TargetInfo"; }
    bool Initialize() override;
    void Shutdown() override;
    void OnPulse() override;
    bool OnIncomingMessage(uint32_t opcode, const void* buffer, uint32_t size) override { return true; }
    void OnCleanUI() override;
    void OnReloadUI() override;
    void OnSetGameState(int gameState) override;
};
