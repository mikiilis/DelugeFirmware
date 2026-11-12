#pragma once

#include "persistence.h"

class Deserializer;
class Serializer;

namespace loop_station {
Status writeLoopStationSettings(Serializer&, const PersistenceState&);
Status readLoopStationSettings(Deserializer&, PersistenceState&);
} // namespace loop_station
