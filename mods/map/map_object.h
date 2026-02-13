/**
 * @file map_object.h
 * @brief MapObject class hierarchy — ported from MapObject.h
 * @date 2026-02-09
 *
 * All CXStr references replaced with std::string.
 * Spawn member access uses SpawnAccess:: namespace.
 */

#pragma once

#include "map.h"

#include <string>
#include <string_view>
#include <vector>
#include <memory>

//============================================================================

class MapObject
{
public:
	MapObject();
	virtual ~MapObject();

	virtual void PostInit();
	virtual void Update(bool forced);

	std::string FormatString(const char* str);
	virtual MapFilter GetMapFilter() const;

	virtual bool CanDisplayObject() const;

	void SetText(std::string_view text);
	std::string GetText() const { return m_text; }
	void SetColor(MQColor color);

	void SetHighlight(bool highlight) { m_highlight = highlight; }
	void SetPosition(float x, float y, float z) { SetPosition(CVector3{ x, y, z }); }
	void SetPosition(const CVector3& pos);
	CVector3 GetPosition() const { return m_pos; }

	MapObject* GetNext() const { return m_pNext; }
	MapObject* GetPrev() const { return m_pLast; }

	virtual SPAWNINFO* GetSpawn() const { return nullptr; }
	virtual GROUNDITEM* GetGroundItem() const { return nullptr; }

protected:
	virtual void HandleFormatSpecifier(char spec, std::string& output);

	void GenerateLabel();

	void GenerateMarker();
	void UpdateMarker();
	void RemoveMarker();

	CVector3              m_pos;
	float                 m_heading = 0.0f;

private:
	void MakeTriangleMarker();
	void MakeSquareMarker();
	void MakeDiamondMarker();
	void MakeRingMarker();

protected:
	std::string           m_text;
	MQColor               m_color;
	MapViewLabel*         m_label = nullptr;
	MapViewLine*          m_vector = nullptr;
	bool                  m_highlight = false;

private:
	MarkerType            m_marker = MarkerType::None;
	uint32_t              m_markerSize = 0;
	std::vector<MapViewLine*> m_markerLines;
	MapObject*            m_pLast = nullptr;
	MapObject*            m_pNext = nullptr;
};

//============================================================================

class MapObjectSpawn : public MapObject
{
public:
	MapObjectSpawn(SPAWNINFO* pSpawn, bool Explicit);
	virtual ~MapObjectSpawn();

	virtual void PostInit() override;
	virtual void Update(bool forced) override;

	virtual MapFilter GetMapFilter() const override;
	virtual bool CanDisplayObject() const override;
	virtual SPAWNINFO* GetSpawn() const { return m_spawn; }

	MQColor GetSpawnColor() const;

private:
	virtual void HandleFormatSpecifier(char spec, std::string& output) override;

	void GenerateVector();
	void UpdateVector();
	void RemoveVector();

private:
	SPAWNINFO* m_spawn = nullptr;
	eSpawnType m_type = NONE;
	bool       m_explicit = false;
};

//============================================================================

class MapObjectGroundSpawn : public MapObject
{
public:
	MapObjectGroundSpawn(EQGroundItem* pGroundItem);
	virtual ~MapObjectGroundSpawn();

	virtual void PostInit() override;
	virtual void Update(bool forced) override;

	virtual MapFilter GetMapFilter() const override;
	virtual bool CanDisplayObject() const override;

	virtual GROUNDITEM* GetGroundItem() const override { return m_groundItem; }

private:
	virtual void HandleFormatSpecifier(char spec, std::string& output) override;

private:
	GROUNDITEM* m_groundItem = nullptr;
	std::string m_friendlyName;
};

//============================================================================

MapObject* MakeMapObject(SPAWNINFO* pSpawn, bool Explicit = false);
MapObject* FindMapObject(SPAWNINFO* pSpawn);

MapObject* MakeMapObject(EQGroundItem* pGroundItem);
MapObject* FindMapObject(EQGroundItem* pGroundItem);

void MapObjects_Clear();

MapObject* GetMapObjectForLabel(MAPLABEL* pLabel);

//============================================================================

class MapCircle
{
public:
	static constexpr int CIRCLE_ANGLESIZE = 10;
	static constexpr int CIRCLE_NUM_SEGMENTS = 360 / CIRCLE_ANGLESIZE;

	MapCircle();
	~MapCircle();

	void Clear();
	void UpdateCircle(MQColor Color, float Radius, float X, float Y, float Z);

private:
	bool m_initialized = false;
	MapViewLine* m_components[CIRCLE_NUM_SEGMENTS];
};

//============================================================================

struct MapLocParams
{
	float   lineSize = 10.f;
	float   width = 2.f;
	MQColor color = MQColor(255, 0, 0);
	float   circleRadius = 0.f;
	MQColor circleColor = MQColor(0, 0, 255);

	std::string MakeCommandString();
};
extern MapLocParams gDefaultMapLocParams;

class MapObjectMapLoc;

class MapLocTemplate
{
public:
	MapLocTemplate(const MapLocParams& params, const std::string& label,
		const std::string& tag, const CVector3& pos, bool isDefault);
	~MapLocTemplate();

	int GetIndex() const { return m_index; }
	void SetIndex(int index);

	const std::string& GetLabelText() const { return m_label; }

	void CreateMapObject();

	void UpdateFromParams(const MapLocParams& params);
	const MapLocParams& GetParams() const { return m_mapLocParams; }

	const CVector3& GetPosition() const { return m_pos; }

	bool IsCreatedFromDefaults() const { return m_isCreatedFromDefaultLoc; }
	void SetCreatedFromDefaults(bool isCreatedFromDefaults) { m_isCreatedFromDefaultLoc = isCreatedFromDefaults; }

	void SetLabel(const std::string& labelText);
	const std::string& GetTag() const { return m_tag; }

	void SetSelected(bool selected);
	bool IsSelected() const { return m_isSelected; }

	void OnMapObjectRemoved() { m_mapObject = nullptr; }

private:
	void UpdateLabel();

private:
	int                   m_index = -1;
	MapLocParams          m_mapLocParams;
	std::string           m_label;
	std::string           m_tag;
	CVector3              m_pos;
	bool                  m_isCreatedFromDefaultLoc = false;
	MapObjectMapLoc*      m_mapObject = nullptr;
	bool                  m_isSelected = false;
};

class MapObjectMapLoc : public MapObject
{
public:
	MapObjectMapLoc(MapLocTemplate* pMapLoc);
	virtual ~MapObjectMapLoc();

	virtual void PostInit() override;
	virtual void Update(bool forced) override;
	virtual bool CanDisplayObject() const override { return true; }

private:
	void UpdateMapObject();
	void RemoveMapObject();

	const MapLocParams& GetParams() const { return m_mapLoc->GetParams(); }

	bool                  m_initialized = false;
	MapLocTemplate*       m_mapLoc;
	std::vector<MapViewLine*> m_lines;
	MapCircle             m_circle;
};

extern std::vector<std::unique_ptr<MapLocTemplate>> gMapLocTemplates;
extern MapLocParams gOverrideMapLocParams;

void InitDefaultMapLocParams();
void UpdateDefaultMapLocInstances();
void ResetMapLocOverrides();

MapLocTemplate* GetMapLocTemplateByTag(std::string_view tag);
MapLocTemplate* GetMapLocByIndex(int index);

void CreateAllMapLocs();
void DeleteAllMapLocs();

void AddMapLoc(std::unique_ptr<MapLocTemplate> mapLoc);
void DeleteMapLoc(MapLocTemplate* mapLoc);
void DeleteSelectedMapLocs();
