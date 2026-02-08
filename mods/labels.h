/**
 * @file labels.h
 * @brief LabelsOverride mod — hooks label, gauge, and stat functions to display
 *        server-authoritative values from EdgeStat packets.
 * @date 2026-02-07
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "mod_interface.h"

// Standard EQ class title lookup: returns the appropriate title for a given
// class ID (eqlib PlayerClass enum: Warrior=1..Berserker=16) and level.
const char* GetClassTitle(int classId, int level);

class LabelsOverride : public IMod
{
public:
    const char* GetName() const override;
    bool        Initialize() override;
    void        Shutdown() override;
    void        OnPulse() override;
    bool        OnIncomingMessage(uint32_t opcode, const void* buffer, uint32_t size) override;
};
