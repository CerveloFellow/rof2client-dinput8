/**
 * @file map_mod.h
 * @brief MapMod — IMod implementation for the map plugin.
 * @date 2026-02-09
 *
 * Installs a PostDraw hook on MapViewMap to render spawn dots and labels
 * on the in-game map window. Handles spawn/ground item lifecycle and
 * game state transitions.
 */

#pragma once

#include "../mod_interface.h"

class MapMod : public IMod
{
public:
	const char* GetName() const override { return "Map"; }

	bool Initialize() override;
	void Shutdown() override;

	void OnPulse() override;
	bool OnIncomingMessage(uint32_t opcode, const void* buffer, uint32_t size) override;

	void OnAddSpawn(void* pSpawn) override;
	void OnRemoveSpawn(void* pSpawn) override;
	void OnAddGroundItem(void* pItem) override;
	void OnRemoveGroundItem(void* pItem) override;

	void OnSetGameState(int gameState) override;
	void OnCleanUI() override;
	void OnReloadUI() override;

private:
	bool m_mapActive = false;  // true after first MapGenerate
};
