//////////////////////////////////////////////////////////////////////
// This file is part of Remere's Map Editor
//////////////////////////////////////////////////////////////////////
// Remere's Map Editor is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Remere's Map Editor is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.
//////////////////////////////////////////////////////////////////////

#include "main.h"

#include "ground_brush.h"
#include "items.h"
#include "basemap.h"

static thread_local std::set<Position> processing_tiles;

uint32_t GroundBrush::border_types[256];

int AutoBorder::edgeNameToID(const std::string& edgename) {
	if (edgename == "n") {
		return NORTH_HORIZONTAL;
	} else if (edgename == "w") {
		return WEST_HORIZONTAL;
	} else if (edgename == "s") {
		return SOUTH_HORIZONTAL;
	} else if (edgename == "e") {
		return EAST_HORIZONTAL;
	} else if (edgename == "cnw") {
		return NORTHWEST_CORNER;
	} else if (edgename == "cne") {
		return NORTHEAST_CORNER;
	} else if (edgename == "csw") {
		return SOUTHWEST_CORNER;
	} else if (edgename == "cse") {
		return SOUTHEAST_CORNER;
	} else if (edgename == "dnw") {
		return NORTHWEST_DIAGONAL;
	} else if (edgename == "dne") {
		return NORTHEAST_DIAGONAL;
	} else if (edgename == "dsw") {
		return SOUTHWEST_DIAGONAL;
	} else if (edgename == "dse") {
		return SOUTHEAST_DIAGONAL;
	}
	return BORDER_NONE;
}

bool AutoBorder::load(pugi::xml_node node, wxArrayString& warnings, GroundBrush* owner, uint16_t ground_equivalent) {
	ASSERT(ground ? ground_equivalent != 0 : true);

	pugi::xml_attribute attribute;

	bool optionalBorder = false;
	if ((attribute = node.attribute("type"))) {
		if (std::string(attribute.as_string()) == "optional") {
			optionalBorder = true;
		}
	}

	if ((attribute = node.attribute("group"))) {
		group = attribute.as_ushort();
	}

	for (pugi::xml_node childNode = node.first_child(); childNode; childNode = childNode.next_sibling()) {
		if (!(attribute = childNode.attribute("item"))) {
			continue;
		}

		uint16_t itemid = attribute.as_ushort();
		if (!(attribute = childNode.attribute("edge"))) {
			continue;
		}

		const std::string& orientation = attribute.as_string();

		ItemType& it = g_items[itemid];
		if (it.id == 0) {
			warnings.push_back("Invalid item ID " + std::to_string(itemid) + " for border " + std::to_string(id));
			continue;
		}

		if (ground) { // We are a ground border
			it.group = ITEM_GROUP_NONE;
			it.ground_equivalent = ground_equivalent;
			it.brush = owner;

			ItemType& it2 = g_items[ground_equivalent];
			it2.has_equivalent = it2.id != 0;
		}

		it.alwaysOnBottom = true; // Never-ever place other items under this, will confuse the user something awful.
		it.isBorder = true;
		it.isOptionalBorder = it.isOptionalBorder ? true : optionalBorder;
		if (group && !it.border_group) {
			it.border_group = group;
		}

		int32_t edge_id = edgeNameToID(orientation);
		if (edge_id != BORDER_NONE) {
			tiles[edge_id] = itemid;
			if (it.border_alignment == BORDER_NONE) {
				it.border_alignment = ::BorderType(edge_id);
			}
		}
	}
	return true;
}

GroundBrush::GroundBrush() :
	z_order(0),
	has_zilch_outer_border(false),
	has_zilch_inner_border(false),
	has_outer_border(false),
	has_inner_border(false),
	optional_border(nullptr),
	use_only_optional(false),
	randomize(true),
	total_chance(0) {
	////
}

GroundBrush::~GroundBrush() {
	for (BorderBlock* borderBlock : borders) {
		if (borderBlock->autoborder) {
			for (SpecificCaseBlock* specificCaseBlock : borderBlock->specific_cases) {
				delete specificCaseBlock;
			}

			if (borderBlock->autoborder->ground) {
				delete borderBlock->autoborder;
			}
		}
		delete borderBlock;
	}
	borders.clear();
}

bool GroundBrush::load(pugi::xml_node node, wxArrayString& warnings) {
	pugi::xml_attribute attribute;
	if ((attribute = node.attribute("lookid"))) {
		look_id = attribute.as_ushort();
	}

	if ((attribute = node.attribute("server_lookid"))) {
		look_id = g_items[attribute.as_ushort()].clientID;
	}

	if ((attribute = node.attribute("z-order"))) {
		z_order = attribute.as_int();
	}

	if ((attribute = node.attribute("solo_optional"))) {
		use_only_optional = attribute.as_bool();
	}

	if ((attribute = node.attribute("randomize"))) {
		randomize = attribute.as_bool();
	}

	for (pugi::xml_node childNode = node.first_child(); childNode; childNode = childNode.next_sibling()) {
		const std::string& childName = as_lower_str(childNode.name());
		if (childName == "item") {
			uint16_t itemId = childNode.attribute("id").as_ushort();
			int32_t chance = childNode.attribute("chance").as_int();

			ItemType& it = g_items[itemId];
			if (it.id == 0) {
				warnings.push_back("\nInvalid item id " + std::to_string(itemId));
				return false;
			}

			if (!it.isGroundTile()) {
				warnings.push_back("\nItem " + std::to_string(itemId) + " is not ground item.");
				return false;
			}

			if (it.brush && it.brush != this) {
				warnings.push_back("\nItem " + std::to_string(itemId) + " can not be member of two brushes");
				return false;
			}

			it.brush = this;
			total_chance += chance;

			ItemChanceBlock ci;
			ci.id = itemId;
			ci.chance = total_chance;
			border_items.push_back(ci);
		} else if (childName == "optional") {
			// Mountain border!
			if (optional_border) {
				warnings.push_back("\nDuplicate optional borders!");
				continue;
			}

			if ((attribute = childNode.attribute("ground_equivalent"))) {
				uint16_t ground_equivalent = attribute.as_ushort();

				// Load from inline definition
				ItemType& it = g_items[ground_equivalent];
				if (it.id == 0) {
					warnings.push_back("Invalid id of ground dependency equivalent item.\n");
					continue;
				} else if (!it.isGroundTile()) {
					warnings.push_back("Ground dependency equivalent is not a ground item.\n");
					continue;
				} else if (it.brush && it.brush != this) {
					warnings.push_back("Ground dependency equivalent does not use the same brush as ground border.\n");
					continue;
				}

				AutoBorder* autoBorder = newd AutoBorder(0); // Empty id basically
				autoBorder->load(childNode, warnings, this, ground_equivalent);
				optional_border = autoBorder;
			} else {
				// Load from ID
				if (!(attribute = childNode.attribute("id"))) {
					warnings.push_back("\nMissing tag id for border node");
					continue;
				}

				uint16_t id = attribute.as_ushort();
				auto it = g_brushes.borders.find(id);
				if (it == g_brushes.borders.end() || !it->second) {
					warnings.push_back("\nCould not find border id " + std::to_string(id));
					continue;
				}

				optional_border = it->second;
			}
		} else if (childName == "border") {
			AutoBorder* autoBorder;
			if (!(attribute = childNode.attribute("id"))) {
				if (!(attribute = childNode.attribute("ground_equivalent"))) {
					continue;
				}

				uint16_t ground_equivalent = attribute.as_ushort();
				ItemType& it = g_items[ground_equivalent];
				if (it.id == 0) {
					warnings.push_back("Invalid id of ground dependency equivalent item.\n");
				}

				if (!it.isGroundTile()) {
					warnings.push_back("Ground dependency equivalent is not a ground item.\n");
				}

				if (it.brush && it.brush != this) {
					warnings.push_back("Ground dependency equivalent does not use the same brush as ground border.\n");
				}

				autoBorder = newd AutoBorder(0); // Empty id basically
				autoBorder->load(childNode, warnings, this, ground_equivalent);
			} else {
				int32_t id = attribute.as_int();
				if (id == 0) {
					autoBorder = nullptr;
				} else {
					auto it = g_brushes.borders.find(id);
					if (it == g_brushes.borders.end() || !it->second) {
						warnings.push_back("\nCould not find border id " + std::to_string(id));
						continue;
					}
					autoBorder = it->second;
				}
			}

			BorderBlock* borderBlock = newd BorderBlock;
			borderBlock->super = false;
			borderBlock->autoborder = autoBorder;

			if ((attribute = childNode.attribute("to"))) {
				const std::string& value = attribute.as_string();
				if (value == "all") {
					borderBlock->to = 0xFFFFFFFF;
				} else if (value == "none") {
					borderBlock->to = 0;
				} else {
					Brush* tobrush = g_brushes.getBrush(value);
					if (!tobrush) {
						warnings.push_back("To brush " + wxstr(value) + " doesn't exist.");
						continue;
					}
					borderBlock->to = tobrush->getID();
				}
			} else {
				borderBlock->to = 0xFFFFFFFF;
			}

			if ((attribute = childNode.attribute("super")) && attribute.as_bool()) {
				borderBlock->super = true;
			}

			if ((attribute = childNode.attribute("align"))) {
				const std::string& value = attribute.as_string();
				if (value == "outer") {
					borderBlock->outer = true;
				} else if (value == "inner") {
					borderBlock->outer = false;
				} else {
					borderBlock->outer = true;
				}
			}

			if (borderBlock->outer) {
				if (borderBlock->to == 0) {
					has_zilch_outer_border = true;
				} else {
					has_outer_border = true;
				}
			} else {
				if (borderBlock->to == 0) {
					has_zilch_inner_border = true;
				} else {
					has_inner_border = true;
				}
			}

			for (pugi::xml_node subChildNode = childNode.first_child(); subChildNode; subChildNode = subChildNode.next_sibling()) {
				if (as_lower_str(subChildNode.name()) != "specific") {
					continue;
				}

				SpecificCaseBlock* specificCaseBlock = nullptr;
				for (pugi::xml_node superChildNode = subChildNode.first_child(); superChildNode; superChildNode = superChildNode.next_sibling()) {
					const std::string& superChildName = as_lower_str(superChildNode.name());
					if (superChildName == "conditions") {
						for (pugi::xml_node conditionChild = superChildNode.first_child(); conditionChild; conditionChild = conditionChild.next_sibling()) {
							const std::string& conditionName = as_lower_str(conditionChild.name());
							if (conditionName == "match_border") {
								if (!(attribute = conditionChild.attribute("id"))) {
									continue;
								}

								int32_t border_id = attribute.as_int();
								if (!(attribute = conditionChild.attribute("edge"))) {
									continue;
								}

								int32_t edge_id = AutoBorder::edgeNameToID(attribute.as_string());
								auto it = g_brushes.borders.find(border_id);
								if (it == g_brushes.borders.end()) {
									warnings.push_back("Unknown border id in specific case match block " + std::to_string(border_id));
									continue;
								}

								AutoBorder* autoBorder = it->second;
								ASSERT(autoBorder != nullptr);

								uint32_t match_itemid = autoBorder->tiles[edge_id];
								if (!specificCaseBlock) {
									specificCaseBlock = newd SpecificCaseBlock();
								}
								specificCaseBlock->items_to_match.push_back(match_itemid);
							} else if (conditionName == "match_group") {
								if (!(attribute = conditionChild.attribute("group"))) {
									continue;
								}

								uint16_t group = attribute.as_ushort();
								if (!(attribute = conditionChild.attribute("edge"))) {
									continue;
								}

								int32_t edge_id = AutoBorder::edgeNameToID(attribute.as_string());
								if (!specificCaseBlock) {
									specificCaseBlock = newd SpecificCaseBlock();
								}

								specificCaseBlock->match_group = group;
								specificCaseBlock->group_match_alignment = ::BorderType(edge_id);
								specificCaseBlock->items_to_match.push_back(group);
							} else if (conditionName == "match_item") {
								if (!(attribute = conditionChild.attribute("id"))) {
									continue;
								}

								int32_t match_itemid = attribute.as_int();
								if (!specificCaseBlock) {
									specificCaseBlock = newd SpecificCaseBlock();
								}

								specificCaseBlock->match_group = 0;
								specificCaseBlock->items_to_match.push_back(match_itemid);
							}
						}
					} else if (superChildName == "actions") {
						for (pugi::xml_node actionChild = superChildNode.first_child(); actionChild; actionChild = actionChild.next_sibling()) {
							const std::string& actionName = as_lower_str(actionChild.name());
							if (actionName == "replace_border") {
								if (!(attribute = actionChild.attribute("id"))) {
									continue;
								}

								int32_t border_id = attribute.as_int();
								if (!(attribute = actionChild.attribute("edge"))) {
									continue;
								}

								int32_t edge_id = AutoBorder::edgeNameToID(attribute.as_string());
								if (!(attribute = actionChild.attribute("with"))) {
									continue;
								}

								int32_t with_id = attribute.as_int();
								auto itt = g_brushes.borders.find(border_id);
								if (itt == g_brushes.borders.end()) {
									warnings.push_back("Unknown border id in specific case match block " + std::to_string(border_id));
									continue;
								}

								AutoBorder* autoBorder = itt->second;
								ASSERT(autoBorder != nullptr);

								ItemType& it = g_items[with_id];
								if (it.id == 0) {
									return false;
								}

								it.isBorder = true;
								if (!specificCaseBlock) {
									specificCaseBlock = newd SpecificCaseBlock();
								}

								specificCaseBlock->to_replace_id = autoBorder->tiles[edge_id];
								specificCaseBlock->with_id = with_id;
							} else if (actionName == "replace_item") {
								if (!(attribute = actionChild.attribute("id"))) {
									continue;
								}

								int32_t to_replace_id = attribute.as_int();
								if (!(attribute = actionChild.attribute("with"))) {
									continue;
								}

								int32_t with_id = attribute.as_int();
								ItemType& it = g_items[with_id];
								if (it.id == 0) {
									return false;
								}

								it.isBorder = true;
								if (!specificCaseBlock) {
									specificCaseBlock = newd SpecificCaseBlock();
								}

								specificCaseBlock->to_replace_id = to_replace_id;
								specificCaseBlock->with_id = with_id;
							} else if (actionName == "delete_borders") {
								if (!specificCaseBlock) {
									specificCaseBlock = newd SpecificCaseBlock();
								}
								specificCaseBlock->delete_all = true;
							}
						}
					}
				}
				if (specificCaseBlock) {
					if (attribute = subChildNode.attribute("keep_border")) {
						specificCaseBlock->keepBorder = attribute.as_bool();
					}

					borderBlock->specific_cases.push_back(specificCaseBlock);
				}
			}
			borders.push_back(borderBlock);
		} else if (childName == "friend") {
			const std::string& name = childNode.attribute("name").as_string();
			if (!name.empty()) {
				if (name == "all") {
					friends.push_back(0xFFFFFFFF);
				} else {
					Brush* brush = g_brushes.getBrush(name);
					if (brush) {
						friends.push_back(brush->getID());
					} else {
						warnings.push_back("Brush '" + wxstr(name) + "' is not defined.");
					}
				}
			}
			hate_friends = false;
		} else if (childName == "enemy") {
			const std::string& name = childNode.attribute("name").as_string();
			if (!name.empty()) {
				if (name == "all") {
					friends.push_back(0xFFFFFFFF);
				} else {
					Brush* brush = g_brushes.getBrush(name);
					if (brush) {
						friends.push_back(brush->getID());
					} else {
						warnings.push_back("Brush '" + wxstr(name) + "' is not defined.");
					}
				}
			}
			hate_friends = true;
		} else if (childName == "clear_borders") {
			for (std::vector<BorderBlock*>::iterator it = borders.begin();
				 it != borders.end();
				 ++it) {
				BorderBlock* bb = *it;
				if (bb->autoborder) {
					for (std::vector<SpecificCaseBlock*>::iterator specific_iter = bb->specific_cases.begin(); specific_iter != bb->specific_cases.end(); ++specific_iter) {
						delete *specific_iter;
					}
					if (bb->autoborder->ground) {
						delete bb->autoborder;
					}
				}
				delete bb;
			}
			borders.clear();
		} else if (childName == "clear_friends") {
			friends.clear();
			hate_friends = false;
		}
	}

	if (total_chance == 0) {
		randomize = false;
	}

	return true;
}

void GroundBrush::undraw(BaseMap* map, Tile* tile) {
	ASSERT(tile);
	if (tile->hasGround() && tile->ground->getGroundBrush() == this) {
		delete tile->ground;
		tile->ground = nullptr;
	}
}

void GroundBrush::draw(BaseMap* map, Tile* tile, void* parameter) {
	ASSERT(tile);
	if (border_items.empty()) {
		return;
	}

	if (parameter != nullptr) {
		std::pair<bool, GroundBrush*>& param = *reinterpret_cast<std::pair<bool, GroundBrush*>*>(parameter);
		GroundBrush* other = tile->getGroundBrush();
		if (param.first) { // Volatile? :)
			if (other != nullptr) {
				return;
			}
		} else if (other != param.second) {
			return;
		}
	}
	
	// Get a random ground ID
	int chance = random(1, total_chance);
	uint16_t id = 0;
	for (std::vector<ItemChanceBlock>::const_iterator it = border_items.begin(); it != border_items.end(); ++it) {
		if (chance < it->chance) {
			id = it->id;
			break;
		}
	}
	if (id == 0) {
		id = border_items.front().id;
	}

	// Create the ground item
	Item* groundItem = Item::Create(id);
	
	// Always properly handle ground items
	// First remove any existing ground
	if (tile->ground) {
		delete tile->ground;
		tile->ground = nullptr;
	}
	
	// Also remove any items that should be grounds but were placed incorrectly
	ItemVector::iterator it = tile->items.begin();
	while (it != tile->items.end()) {
		Item* item = *it;
		if (item && (item->isGroundTile() || item->getGroundEquivalent() != 0)) {
#ifdef __WXDEBUG__
			printf("DEBUG: Removing misplaced ground item with ID %d from tile items\n", item->getID());
#endif
			delete item;
			it = tile->items.erase(it);
		} else {
			++it;
		}
	}
	
	// Then set the new ground
	tile->ground = groundItem;
}

const GroundBrush::BorderBlock* GroundBrush::getBrushTo(GroundBrush* first, GroundBrush* second) {
	// printf("Border from %s to %s : ", first->getName().c_str(), second->getName().c_str());
	if (first) {
		if (second) {
			if (first->getZ() < second->getZ() && second->hasOuterBorder()) {
				if (first->hasInnerBorder()) {
					for (std::vector<BorderBlock*>::iterator it = first->borders.begin(); it != first->borders.end(); ++it) {
						BorderBlock* bb = *it;
						if (bb->outer) {
							continue;
						} else if (bb->to == second->getID() || bb->to == 0xFFFFFFFF) {
							// printf("%d\n", bb->autoborder);
							return bb;
						}
					}
				}
				for (std::vector<BorderBlock*>::iterator it = second->borders.begin(); it != second->borders.end(); ++it) {
					BorderBlock* bb = *it;
					if (!bb->outer) {
						continue;
					} else if (bb->to == first->getID()) {
						// printf("%d\n", bb->autoborder);
						return bb;
					} else if (bb->to == 0xFFFFFFFF) {
						// printf("%d\n", bb->autoborder);
						return bb;
					}
				}
			} else if (first->hasInnerBorder()) {
				for (std::vector<BorderBlock*>::iterator it = first->borders.begin(); it != first->borders.end(); ++it) {
					BorderBlock* bb = *it;
					if (bb->outer) {
						continue;
					} else if (bb->to == second->getID()) {
						// printf("%d\n", bb->autoborder);
						return bb;
					} else if (bb->to == 0xFFFFFFFF) {
						// printf("%d\n", bb->autoborder);
						return bb;
					}
				}
			}
		} else if (first->hasInnerZilchBorder()) {
			for (std::vector<BorderBlock*>::iterator it = first->borders.begin(); it != first->borders.end(); ++it) {
				BorderBlock* bb = *it;
				if (bb->outer) {
					continue;
				} else if (bb->to == 0) {
					// printf("%d\n", bb->autoborder);
					return bb;
				}
			}
		}
	} else if (second && second->hasOuterZilchBorder()) {
		for (std::vector<BorderBlock*>::iterator it = second->borders.begin(); it != second->borders.end(); ++it) {
			BorderBlock* bb = *it;
			if (!bb->outer) {
				continue;
			} else if (bb->to == 0) {
				// printf("%d\n", bb->autoborder);
				return bb;
			}
		}
	}
	// printf("None\n");
	return nullptr;
}

void GroundBrush::doBorders(BaseMap* map, Tile* tile) {
	// Add recursion guard
	if (!tile || processing_tiles.find(tile->getPosition()) != processing_tiles.end()) {
		return;
	}
	processing_tiles.insert(tile->getPosition());
	
	// Early exit for custom border mode
	if (g_settings.getBoolean(Config::CUSTOM_BORDER_ENABLED) && g_settings.getBoolean(Config::USE_AUTOMAGIC)) {
		// Get the custom border ID
		int customBorderId = g_settings.getInteger(Config::CUSTOM_BORDER_ID);
		if (customBorderId <= 0) {
			// Invalid border ID, fall back to normal border handling
			processing_tiles.erase(tile->getPosition());
			return;
		}
		
		// Check if we need to clean existing borders
		if (g_settings.getBoolean(Config::SAME_GROUND_TYPE_BORDER)) {
			// Only clean borders matching the custom border ID
			ItemVector::iterator it = tile->items.begin();
			while (it != tile->items.end()) {
				if ((*it)->isBorder()) {
					// Go through all borders in the map and check if this item matches any
					bool found = false;
					for (auto& borderPair : g_brushes.borders) {
						if (borderPair.second && borderPair.second->hasItemId((*it)->getID())) {
							found = true;
							break;
						}
					}
					
					if (found) {
						delete *it;
						it = tile->items.erase(it);
					} else {
						++it;
					}
				} else {
					++it;
				}
			}
		} else {
			// Clean all borders
			tile->cleanBorders();
		}
		
		// Look up the border in the borders container
		auto it = g_brushes.borders.find(customBorderId);
		if (it == g_brushes.borders.end() || !it->second) {
			processing_tiles.erase(tile->getPosition());
			return; // Border ID not found
		}
		
		AutoBorder* customBorder = it->second;
		
		// Get neighboring tiles to determine border pattern
		const Position& position = tile->getPosition();
		uint32_t x = position.x;
		uint32_t y = position.y;
		uint32_t z = position.z;
		
		// Check the 8 surrounding tiles for ground, to apply borders where there's no ground
		// We mark each position as true if it needs a border (no ground or different ground)
		bool borders[8] = {false, false, false, false, false, false, false, false};
		
		// NW, N, NE, W, E, SW, S, SE
		static const std::pair<int, int> offsets[8] = {
			{-1, -1}, {0, -1}, {1, -1},
			{-1,  0},          {1,  0},
			{-1,  1}, {0,  1}, {1,  1}
		};
		
		uint32_t tiledata = 0;
		
		// Get the ground brush from the current tile
		GroundBrush* tileBrush = nullptr;
		if (tile->ground) {
			tileBrush = tile->ground->getGroundBrush();
		}
		
		// Check all 8 surrounding positions
		for (int i = 0; i < 8; ++i) {
			Tile* neighbor = map->getTile(x + offsets[i].first, y + offsets[i].second, z);
			
			// First check for walls if walls repel borders is enabled
			if (g_settings.getBoolean(Config::WALLS_REPEL_BORDERS) && neighbor) {
				bool hasWall = false;
				for (Item* item : neighbor->items) {
					if (item->isWall()) {
						hasWall = true;
						break;
					}
				}
				if (hasWall) {
					continue; // Skip this neighbor if it has a wall
				}
			}
			
			// Add border if:
			// - The neighbor is missing, or
			// - The neighbor has no ground, or
			// - The neighbor has a different ground brush
			if (!neighbor || !neighbor->ground) {
				borders[i] = true;
				tiledata |= (1 << i);
			} else {
				GroundBrush* neighborBrush = neighbor->ground->getGroundBrush();
				if (tileBrush && neighborBrush && tileBrush->getID() != neighborBrush->getID()) {
					borders[i] = true;
					tiledata |= (1 << i);
				}
			}
		}
		
		// Border to terrain conversion - this is the logic for determining which
		// borders to draw based on the surrounding tiles
		BorderType directions[4] = {
			static_cast<BorderType>((border_types[tiledata] & 0x000000FF) >> 0),
			static_cast<BorderType>((border_types[tiledata] & 0x0000FF00) >> 8),
			static_cast<BorderType>((border_types[tiledata] & 0x00FF0000) >> 16),
			static_cast<BorderType>((border_types[tiledata] & 0xFF000000) >> 24)
		};
		
		// Apply the appropriate borders
		for (int i = 0; i < 4; ++i) {
			BorderType direction = directions[i];
			if (direction == BORDER_NONE) {
				continue;
			}
			
			if (customBorder->tiles[direction]) {
				Item* borderItem = Item::Create(customBorder->tiles[direction]);
				if (borderItem) {
					tile->addBorderItem(borderItem);
				}
			} else {
				// Handle diagonal cases by creating corner pieces from horizontal pieces
				// Only if we have the required horizontal pieces
				bool addedDiagonal = false;
				
				if (direction == NORTHWEST_DIAGONAL && 
					customBorder->tiles[WEST_HORIZONTAL] && customBorder->tiles[NORTH_HORIZONTAL]) {
					Item* borderItem1 = Item::Create(customBorder->tiles[WEST_HORIZONTAL]);
					Item* borderItem2 = Item::Create(customBorder->tiles[NORTH_HORIZONTAL]);
					if (borderItem1 && borderItem2) {
						tile->addBorderItem(borderItem1);
						tile->addBorderItem(borderItem2);
						addedDiagonal = true;
					} else {
						// Clean up if one failed
						delete borderItem1;
						delete borderItem2;
					}
				} else if (direction == NORTHEAST_DIAGONAL && 
					customBorder->tiles[EAST_HORIZONTAL] && customBorder->tiles[NORTH_HORIZONTAL]) {
					Item* borderItem1 = Item::Create(customBorder->tiles[EAST_HORIZONTAL]);
					Item* borderItem2 = Item::Create(customBorder->tiles[NORTH_HORIZONTAL]);
					if (borderItem1 && borderItem2) {
						tile->addBorderItem(borderItem1);
						tile->addBorderItem(borderItem2);
						addedDiagonal = true;
					} else {
						// Clean up if one failed
						delete borderItem1;
						delete borderItem2;
					}
				} else if (direction == SOUTHWEST_DIAGONAL && 
					customBorder->tiles[SOUTH_HORIZONTAL] && customBorder->tiles[WEST_HORIZONTAL]) {
					Item* borderItem1 = Item::Create(customBorder->tiles[SOUTH_HORIZONTAL]);
					Item* borderItem2 = Item::Create(customBorder->tiles[WEST_HORIZONTAL]);
					if (borderItem1 && borderItem2) {
						tile->addBorderItem(borderItem1);
						tile->addBorderItem(borderItem2);
						addedDiagonal = true;
					} else {
						// Clean up if one failed
						delete borderItem1;
						delete borderItem2;
					}
				} else if (direction == SOUTHEAST_DIAGONAL && 
					customBorder->tiles[SOUTH_HORIZONTAL] && customBorder->tiles[EAST_HORIZONTAL]) {
					Item* borderItem1 = Item::Create(customBorder->tiles[SOUTH_HORIZONTAL]);
					Item* borderItem2 = Item::Create(customBorder->tiles[EAST_HORIZONTAL]);
					if (borderItem1 && borderItem2) {
						tile->addBorderItem(borderItem1);
						tile->addBorderItem(borderItem2);
						addedDiagonal = true;
					} else {
						// Clean up if one failed
						delete borderItem1;
						delete borderItem2;
					}
				}
				
				// If we couldn't create the diagonal from horizontals, try to use another border piece
				if (!addedDiagonal) {
					// Try to use any available border piece as a fallback
					for (int j = 1; j <= 8; j++) { // Skip BORDER_NONE (0) and diagonals (9-12)
						if (customBorder->tiles[j]) {
							Item* borderItem = Item::Create(customBorder->tiles[j]);
							if (borderItem) {
								tile->addBorderItem(borderItem);
								break;
							}
						}
					}
				}
			}
		}
		
		// Early return, don't proceed with normal border handling
		processing_tiles.erase(tile->getPosition());
		return;
	}
	
	// Normal border handling below
	static const auto extractGroundBrushFromTile = [](BaseMap* map, uint32_t x, uint32_t y, uint32_t z) -> GroundBrush* {
		Tile* tile = map->getTile(x, y, z);
		if (tile) {
			return tile->getGroundBrush();
		}
		return nullptr;
	};

	// Helper function to check if a tile has a wall that should block borders
	static const auto hasWallOrBlockingItem = [](BaseMap* map, uint32_t x, uint32_t y, uint32_t z) -> bool {
		if (!g_settings.getBoolean(Config::WALLS_REPEL_BORDERS)) {
			return false;
		}
		
		Tile* tile = map->getTile(x, y, z);
		if (!tile) {
			return false;
		}
		
		// Check if the tile has a wall
		for (Item* item : tile->items) {
			if (item->isWall()) {
				return true;
			}
		}
		
		return false;
	};

	ASSERT(tile);

	GroundBrush* borderBrush;
	if (tile->ground) {
		borderBrush = tile->ground->getGroundBrush();
	} else {
		borderBrush = nullptr;
	}

	const Position& position = tile->getPosition();

	uint32_t x = position.x;
	uint32_t y = position.y;
	uint32_t z = position.z;

	// Pair of visited / what border type
	std::pair<bool, GroundBrush*> neighbours[8];
	
	if (g_settings.getBoolean(Config::WALLS_REPEL_BORDERS)) {
		// When walls repel borders, check each neighbor for walls
		neighbours[0] = hasWallOrBlockingItem(map, x - 1, y - 1, z) ? std::make_pair(true, nullptr) : std::make_pair(false, extractGroundBrushFromTile(map, x - 1, y - 1, z));
		neighbours[1] = hasWallOrBlockingItem(map, x, y - 1, z) ? std::make_pair(true, nullptr) : std::make_pair(false, extractGroundBrushFromTile(map, x, y - 1, z));
		neighbours[2] = hasWallOrBlockingItem(map, x + 1, y - 1, z) ? std::make_pair(true, nullptr) : std::make_pair(false, extractGroundBrushFromTile(map, x + 1, y - 1, z));
		neighbours[3] = hasWallOrBlockingItem(map, x - 1, y, z) ? std::make_pair(true, nullptr) : std::make_pair(false, extractGroundBrushFromTile(map, x - 1, y, z));
		neighbours[4] = hasWallOrBlockingItem(map, x + 1, y, z) ? std::make_pair(true, nullptr) : std::make_pair(false, extractGroundBrushFromTile(map, x + 1, y, z));
		neighbours[5] = hasWallOrBlockingItem(map, x - 1, y + 1, z) ? std::make_pair(true, nullptr) : std::make_pair(false, extractGroundBrushFromTile(map, x - 1, y + 1, z));
		neighbours[6] = hasWallOrBlockingItem(map, x, y + 1, z) ? std::make_pair(true, nullptr) : std::make_pair(false, extractGroundBrushFromTile(map, x, y + 1, z));
		neighbours[7] = hasWallOrBlockingItem(map, x + 1, y + 1, z) ? std::make_pair(true, nullptr) : std::make_pair(false, extractGroundBrushFromTile(map, x + 1, y + 1, z));
		
		// If the tile is on the edge of the map, set those neighbors as visited (no borders needed)
		if (x == 0) {
			neighbours[0].first = true;
			neighbours[3].first = true;
			neighbours[5].first = true;
		}
		if (y == 0) {
			neighbours[0].first = true;
			neighbours[1].first = true;
			neighbours[2].first = true;
		}
		
		// Check right and bottom edges - use a safer approach than direct map width/height
		// Right edge - if we can't get a tile at x+1, we're at the edge
		if (!map->getTile(x + 1, y, z)) {
			neighbours[2].first = true;
			neighbours[4].first = true;
			neighbours[7].first = true;
		}
		
		// Bottom edge - if we can't get a tile at y+1, we're at the edge
		if (!map->getTile(x, y + 1, z)) {
			neighbours[5].first = true;
			neighbours[6].first = true;
			neighbours[7].first = true;
		}
	} else {
		// Standard border handling without wall repelling
		if (x == 0) {
			if (y == 0) {
				neighbours[0] = { false, nullptr };
				neighbours[1] = { false, nullptr };
				neighbours[2] = { false, nullptr };
				neighbours[3] = { false, nullptr };
				neighbours[4] = { false, extractGroundBrushFromTile(map, x + 1, y, z) };
				neighbours[5] = { false, nullptr };
				neighbours[6] = { false, extractGroundBrushFromTile(map, x, y + 1, z) };
				neighbours[7] = { false, extractGroundBrushFromTile(map, x + 1, y + 1, z) };
			} else {
				neighbours[0] = { false, nullptr };
				neighbours[1] = { false, extractGroundBrushFromTile(map, x, y - 1, z) };
				neighbours[2] = { false, extractGroundBrushFromTile(map, x + 1, y - 1, z) };
				neighbours[3] = { false, nullptr };
				neighbours[4] = { false, extractGroundBrushFromTile(map, x + 1, y, z) };
				neighbours[5] = { false, nullptr };
				neighbours[6] = { false, extractGroundBrushFromTile(map, x, y + 1, z) };
				neighbours[7] = { false, extractGroundBrushFromTile(map, x + 1, y + 1, z) };
			}
		} else if (y == 0) {
			neighbours[0] = { false, nullptr };
			neighbours[1] = { false, nullptr };
			neighbours[2] = { false, nullptr };
			neighbours[3] = { false, extractGroundBrushFromTile(map, x - 1, y, z) };
			neighbours[4] = { false, extractGroundBrushFromTile(map, x + 1, y, z) };
			neighbours[5] = { false, extractGroundBrushFromTile(map, x - 1, y + 1, z) };
			neighbours[6] = { false, extractGroundBrushFromTile(map, x, y + 1, z) };
			neighbours[7] = { false, extractGroundBrushFromTile(map, x + 1, y + 1, z) };
		} else {
			neighbours[0] = { false, extractGroundBrushFromTile(map, x - 1, y - 1, z) };
			neighbours[1] = { false, extractGroundBrushFromTile(map, x, y - 1, z) };
			neighbours[2] = { false, extractGroundBrushFromTile(map, x + 1, y - 1, z) };
			neighbours[3] = { false, extractGroundBrushFromTile(map, x - 1, y, z) };
			neighbours[4] = { false, extractGroundBrushFromTile(map, x + 1, y, z) };
			neighbours[5] = { false, extractGroundBrushFromTile(map, x - 1, y + 1, z) };
			neighbours[6] = { false, extractGroundBrushFromTile(map, x, y + 1, z) };
			neighbours[7] = { false, extractGroundBrushFromTile(map, x + 1, y + 1, z) };
		}
	}

	static std::vector<const BorderBlock*> specificList;
	specificList.clear();

	std::vector<BorderCluster> borderList;
	for (int32_t i = 0; i < 8; ++i) {
		auto& neighbourPair = neighbours[i];
		if (neighbourPair.first) {
			continue;
		}

		GroundBrush* other = neighbourPair.second;
		if (borderBrush) {
			if (other) {
				if (other->getID() == borderBrush->getID()) {
					continue;
				}

				if (other->hasOuterBorder() || borderBrush->hasInnerBorder()) {
					bool only_mountain = false;
					if (/*!borderBrush->hasInnerBorder() && */ (other->friendOf(borderBrush) || borderBrush->friendOf(other))) {
						if (!other->hasOptionalBorder()) {
							continue;
						}
						only_mountain = true;
					}

					uint32_t tiledata = 0;
					for (int32_t j = i; j < 8; ++j) {
						auto& otherPair = neighbours[j];
						if (!otherPair.first && otherPair.second && otherPair.second->getID() == other->getID()) {
							otherPair.first = true;
							tiledata |= 1 << j;
						}
					}

					if (tiledata != 0) {
						// Add mountain if appropriate!
						if (other->hasOptionalBorder() && tile->hasOptionalBorder()) {
							BorderCluster borderCluster;
							borderCluster.alignment = tiledata;
							borderCluster.z = 0x7FFFFFFF; // Above all other borders
							borderCluster.border = other->optional_border;

							borderList.push_back(borderCluster);
							if (other->useSoloOptionalBorder()) {
								only_mountain = true;
							}
						}

						if (!only_mountain) {
							const BorderBlock* borderBlock = getBrushTo(borderBrush, other);
							if (borderBlock) {
								bool found = false;
								for (BorderCluster& borderCluster : borderList) {
									if (borderCluster.border == borderBlock->autoborder) {
										borderCluster.alignment |= tiledata;
										if (borderCluster.z < other->getZ()) {
											borderCluster.z = other->getZ();
										}

										if (!borderBlock->specific_cases.empty()) {
											if (std::find(specificList.begin(), specificList.end(), borderBlock) == specificList.end()) {
												specificList.push_back(borderBlock);
											}
										}

										found = true;
										break;
									}
								}

								if (!found) {
									BorderCluster borderCluster;
									borderCluster.alignment = tiledata;
									borderCluster.z = other->getZ();
									borderCluster.border = borderBlock->autoborder;

									borderList.push_back(borderCluster);
									if (!borderBlock->specific_cases.empty()) {
										if (std::find(specificList.begin(), specificList.end(), borderBlock) == specificList.end()) {
											specificList.push_back(borderBlock);
										}
									}
								}
							}
						}
					}
				}
			} else if (borderBrush->hasInnerZilchBorder()) {
				// Border against nothing (or undefined tile)
				uint32_t tiledata = 0;
				for (int32_t j = i; j < 8; ++j) {
					auto& otherPair = neighbours[j];
					if (!otherPair.first && !otherPair.second) {
						otherPair.first = true;
						tiledata |= 1 << j;
					}
				}

				if (tiledata != 0) {
					const BorderBlock* borderBlock = getBrushTo(borderBrush, nullptr);
					if (!borderBlock) {
						continue;
					}

					if (borderBlock->autoborder) {
						BorderCluster borderCluster;
						borderCluster.alignment = tiledata;
						borderCluster.z = 5000;
						borderCluster.border = borderBlock->autoborder;

						borderList.push_back(borderCluster);
					}

					if (!borderBlock->specific_cases.empty()) {
						if (std::find(specificList.begin(), specificList.end(), borderBlock) == specificList.end()) {
							specificList.push_back(borderBlock);
						}
					}
				}
				continue;
			}
		} else if (other && other->hasOuterZilchBorder()) {
			// Border against nothing (or undefined tile)
			uint32_t tiledata = 0;
			for (int32_t j = i; j < 8; ++j) {
				auto& otherPair = neighbours[j];
				if (!otherPair.first && otherPair.second && otherPair.second->getID() == other->getID()) {
					otherPair.first = true;
					tiledata |= 1 << j;
				}
			}

			if (tiledata != 0) {
				const BorderBlock* borderBlock = getBrushTo(nullptr, other);
				if (borderBlock) {
					if (borderBlock->autoborder) {
						BorderCluster borderCluster;
						borderCluster.alignment = tiledata;
						borderCluster.z = other->getZ();
						borderCluster.border = borderBlock->autoborder;

						borderList.push_back(borderCluster);
					}

					if (!borderBlock->specific_cases.empty()) {
						if (std::find(specificList.begin(), specificList.end(), borderBlock) == specificList.end()) {
							specificList.push_back(borderBlock);
						}
					}
				}

				// Add mountain if appropriate!
				if (other->hasOptionalBorder() && tile->hasOptionalBorder()) {
					BorderCluster borderCluster;
					borderCluster.alignment = tiledata;
					borderCluster.z = 0x7FFFFFFF; // Above other zilch borders
					borderCluster.border = other->optional_border;

					borderList.push_back(borderCluster);
				} else {
					tile->setOptionalBorder(false);
				}
			}
		}
		// Check tile as done
		neighbourPair.first = true;
	}

	std::sort(borderList.begin(), borderList.end());
	
	// Check if we should preserve existing borders from other ground types
	if (g_settings.getBoolean(Config::SAME_GROUND_TYPE_BORDER)) {
		// If we are preserving borders, we need to identify and remove only
		// the borders that belong to the current ground brush
		ItemVector::iterator it = tile->items.begin();
		while (it != tile->items.end()) {
			if ((*it)->isBorder()) {
				// Check if this border belongs to the current border group
				// by comparing its ID with the IDs in our border list
				bool is_current_border = false;
				for (const auto& borderCluster : borderList) {
					if (borderCluster.border && borderCluster.border->hasItemId((*it)->getID())) {
						is_current_border = true;
						break;
					}
				}
				
				if (is_current_border) {
					// Remove only borders from the current border group
					delete *it;
					it = tile->items.erase(it);
				} else {
					// Keep borders from other border groups
					// We'll move these non-current borders to the top of the stack later
					++it;
				}
			} else {
				// Not a border item, keep it
				++it;
			}
		}
		
		// If we're adding new borders, add them all at the top of the stack
		// Save the border items we're going to add
		std::vector<Item*> borderItemsToAdd;
		
		while (!borderList.empty()) {
			BorderCluster& borderCluster = borderList.back();
			if (!borderCluster.border) {
				borderList.pop_back();
				continue;
			}

			BorderType directions[4] = {
				static_cast<BorderType>((border_types[borderCluster.alignment] & 0x000000FF) >> 0),
				static_cast<BorderType>((border_types[borderCluster.alignment] & 0x0000FF00) >> 8),
				static_cast<BorderType>((border_types[borderCluster.alignment] & 0x00FF0000) >> 16),
				static_cast<BorderType>((border_types[borderCluster.alignment] & 0xFF000000) >> 24)
			};

			for (int32_t i = 0; i < 4; ++i) {
				BorderType direction = directions[i];
				if (direction == BORDER_NONE) {
					break;
				}

				if (borderCluster.border->tiles[direction]) {
					Item* borderItem = Item::Create(borderCluster.border->tiles[direction]);
					borderItemsToAdd.push_back(borderItem);
				} else {
					if (direction == NORTHWEST_DIAGONAL) {
						borderItemsToAdd.push_back(Item::Create(borderCluster.border->tiles[WEST_HORIZONTAL]));
						borderItemsToAdd.push_back(Item::Create(borderCluster.border->tiles[NORTH_HORIZONTAL]));
					} else if (direction == NORTHEAST_DIAGONAL) {
						borderItemsToAdd.push_back(Item::Create(borderCluster.border->tiles[EAST_HORIZONTAL]));
						borderItemsToAdd.push_back(Item::Create(borderCluster.border->tiles[NORTH_HORIZONTAL]));
					} else if (direction == SOUTHWEST_DIAGONAL) {
						borderItemsToAdd.push_back(Item::Create(borderCluster.border->tiles[SOUTH_HORIZONTAL]));
						borderItemsToAdd.push_back(Item::Create(borderCluster.border->tiles[WEST_HORIZONTAL]));
					} else if (direction == SOUTHEAST_DIAGONAL) {
						borderItemsToAdd.push_back(Item::Create(borderCluster.border->tiles[SOUTH_HORIZONTAL]));
						borderItemsToAdd.push_back(Item::Create(borderCluster.border->tiles[EAST_HORIZONTAL]));
					}
				}
			}

			borderList.pop_back();
		}
		
		// Now add all borders to the end of the items list (top of the stack)
		for (Item* borderItem : borderItemsToAdd) {
			tile->items.push_back(borderItem);
		}
		
	} else {
		// Use the standard clean borders method if we're not preserving borders
		tile->cleanBorders();
		
		while (!borderList.empty()) {
			BorderCluster& borderCluster = borderList.back();
			if (!borderCluster.border) {
				borderList.pop_back();
				continue;
			}

			BorderType directions[4] = {
				static_cast<BorderType>((border_types[borderCluster.alignment] & 0x000000FF) >> 0),
				static_cast<BorderType>((border_types[borderCluster.alignment] & 0x0000FF00) >> 8),
				static_cast<BorderType>((border_types[borderCluster.alignment] & 0x00FF0000) >> 16),
				static_cast<BorderType>((border_types[borderCluster.alignment] & 0xFF000000) >> 24)
			};

			for (int32_t i = 0; i < 4; ++i) {
				BorderType direction = directions[i];
				if (direction == BORDER_NONE) {
					break;
				}

				if (borderCluster.border->tiles[direction]) {
					Item* borderItem = Item::Create(borderCluster.border->tiles[direction]);
					tile->addBorderItem(borderItem);
				} else {
					if (direction == NORTHWEST_DIAGONAL) {
						Item* borderItem1 = Item::Create(borderCluster.border->tiles[WEST_HORIZONTAL]);
						Item* borderItem2 = Item::Create(borderCluster.border->tiles[NORTH_HORIZONTAL]);
						tile->addBorderItem(borderItem1);
						tile->addBorderItem(borderItem2);
					} else if (direction == NORTHEAST_DIAGONAL) {
						Item* borderItem1 = Item::Create(borderCluster.border->tiles[EAST_HORIZONTAL]);
						Item* borderItem2 = Item::Create(borderCluster.border->tiles[NORTH_HORIZONTAL]);
						tile->addBorderItem(borderItem1);
						tile->addBorderItem(borderItem2);
					} else if (direction == SOUTHWEST_DIAGONAL) {
						Item* borderItem1 = Item::Create(borderCluster.border->tiles[SOUTH_HORIZONTAL]);
						Item* borderItem2 = Item::Create(borderCluster.border->tiles[WEST_HORIZONTAL]);
						tile->addBorderItem(borderItem1);
						tile->addBorderItem(borderItem2);
					} else if (direction == SOUTHEAST_DIAGONAL) {
						Item* borderItem1 = Item::Create(borderCluster.border->tiles[SOUTH_HORIZONTAL]);
						Item* borderItem2 = Item::Create(borderCluster.border->tiles[EAST_HORIZONTAL]);
						tile->addBorderItem(borderItem1);
						tile->addBorderItem(borderItem2);
					}
				}
			}

			borderList.pop_back();
		}
	}

	for (const BorderBlock* borderBlock : specificList) {
		for (const SpecificCaseBlock* specificCaseBlock : borderBlock->specific_cases) {
			uint32_t matches = 0;
			for (Item* item : tile->items) {
				if (!item->isBorder()) {
					break;
				}

				if (specificCaseBlock->match_group > 0) {
					// printf("Matching %d == %d : %d == %d\n", item->getBorderGroup(), specificCaseBlock->match_group, item->getBorderAlignment(), specificCaseBlock->group_match_alignment);
					if (item->getBorderGroup() == specificCaseBlock->match_group && item->getBorderAlignment() == specificCaseBlock->group_match_alignment) {
						// printf("Successfully matched %d == %d : %d == %d\n", item->getBorderGroup(), specificCaseBlock->match_group, item->getBorderAlignment(), specificCaseBlock->group_match_alignment);
						++matches;
						continue;
					}
				}

				// printf("\tInvestigating first item id:%d\n", item->getID());
				for (uint16_t matchId : specificCaseBlock->items_to_match) {
					if (item->getID() == matchId) {
						// printf("\t\tMatched item id %d\n", item->getID());
						++matches;
					}
				}
			}

			// printf("\t\t%d matches of %d\n", matches, scb->items_to_match.size());
			if (matches >= specificCaseBlock->items_to_match.size()) {
				auto& tileItems = tile->items;
				auto it = tileItems.begin();

				// if delete_all mode, consider the border replaced
				bool replaced = specificCaseBlock->delete_all;

				// Add iteration guard to prevent infinite loops
				int item_iterations = 0;
				const int max_item_iterations = tileItems.size() * 2 + 20; // Safety limit

				while (it != tileItems.end() && item_iterations < max_item_iterations) {
					++item_iterations;
					
					Item* item = *it;
					if (!item->isBorder()) {
						++it;
						continue;
					}

					bool inc = true;
					for (uint16_t matchId : specificCaseBlock->items_to_match) {
						if (item->getID() == matchId) {
							if (!replaced && item->getID() == specificCaseBlock->to_replace_id) {
								// replace the matching border, delete everything else
								item->setID(specificCaseBlock->with_id);
								replaced = true;
							} else {
								if (specificCaseBlock->delete_all || !specificCaseBlock->keepBorder) {
									delete item;
									it = tileItems.erase(it);
									inc = false;
									break;
								}
							}
						}
					}

					if (inc) {
						++it;
					}
				}
			}
		}
	}

	// Remove tile from processing set when done
	processing_tiles.erase(tile->getPosition());
}

// Add a custom method to reset borderize for the auto-magic behavior
void GroundBrush::reborderizeTile(BaseMap* map, Tile* tile) {
	// First, clean any existing borders on the tile
	if (g_settings.getBoolean(Config::SAME_GROUND_TYPE_BORDER)) {
		// When Same Ground Type Border is enabled, we need to identify and remove
		// only the borders that might interfere with the new ground type
		ItemVector::iterator it = tile->items.begin();
		while (it != tile->items.end()) {
			if ((*it)->isBorder() && (*it) != tile->ground) {
				// For each border item, check if it belongs to a non-current border group
				// For now, just remove all borders for simplicity
				delete *it;
				it = tile->items.erase(it);
			} else {
				++it;
			}
		}
	} else {
		tile->cleanBorders();
	}
	
	// Now add new borders
	doBorders(map, tile);
}
