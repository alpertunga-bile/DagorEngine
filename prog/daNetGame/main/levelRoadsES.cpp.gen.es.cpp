#include <daECS/core/internal/ltComponentList.h>
static constexpr ecs::component_t level_roads_get_type();
static ecs::LTComponentList level_roads_component(ECS_HASH("level_roads"), level_roads_get_type(), "prog/daNetGame/main/levelRoadsES.cpp.inl", "", 0);
#include "levelRoadsES.cpp.inl"
ECS_DEF_PULL_VAR(levelRoads);
//built with ECS codegen version 1.0
#include <daECS/core/internal/performQuery.h>
static constexpr ecs::component_t level_roads_get_type(){return ecs::ComponentTypeInfo<splineroads::SplineRoads>::type; }
