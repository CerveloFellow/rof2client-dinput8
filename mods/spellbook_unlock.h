/**
 * @file spellbook_unlock.h
 * @brief SpellbookUnlock mod — bypasses class restrictions on spells, memorization,
 *        and combat abilities for multiclass/classless characters.
 * @date 2026-02-07
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "mod_interface.h"

class SpellbookUnlock : public IMod
{
public:
    const char* GetName() const override;
    bool        Initialize() override;
    void        Shutdown() override;
    void        OnPulse() override;
    bool        OnIncomingMessage(uint32_t opcode, const void* buffer, uint32_t size) override;
};
