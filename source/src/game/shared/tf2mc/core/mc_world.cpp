//========= TF2MC bridge - engine independent core =========//
#include "mc_world.h"

#include <algorithm>
#include <sstream>

namespace mc
{
	//==================================================================
	// BlockRegistry
	//==================================================================
	namespace
	{
		bool Contains( const std::string &s, const char *sub ) { return s.find( sub ) != std::string::npos; }
		bool EndsWith( const std::string &s, const char *suf )
		{
			size_t n = strlen( suf );
			return s.size() >= n && s.compare( s.size() - n, n, suf ) == 0;
		}
		std::string StripNamespace( const std::string &n )
		{
			size_t c = n.find( ':' );
			std::string s = c == std::string::npos ? n : n.substr( c + 1 );
			size_t b = s.find( '[' );   // tolerate "name[state]" strings
			return b == std::string::npos ? s : s.substr( 0, b );
		}
		uint32_t RGB( uint32_t rgb ) { return 0xFF000000u | ( ( rgb & 0xFF ) << 16 ) | ( rgb & 0xFF00 ) | ( ( rgb >> 16 ) & 0xFF ); }

		struct KnownColor { const char *key; uint32_t rgb; };
		const KnownColor kColors[] = {
			{ "grass_block", 0x6DA544 }, { "dirt", 0x86603F }, { "coarse_dirt", 0x77553B }, { "podzol", 0x5B3F1C },
			{ "mycelium", 0x6F6265 }, { "stone", 0x7D7D7D }, { "cobblestone", 0x6E6E6E }, { "deepslate", 0x4D4D50 },
			{ "bedrock", 0x3A3A3A }, { "sandstone", 0xD8CB9B }, { "red_sand", 0xBE6621 }, { "sand", 0xDBCFA3 },
			{ "gravel", 0x837E7D }, { "clay", 0xA0A6B3 }, { "water", 0x3F76E4 }, { "lava", 0xCF5A12 },
			{ "snow", 0xF3FAFA }, { "ice", 0x91B7FD }, { "leaves", 0x3E8A2D }, { "log", 0x6B5333 }, { "wood", 0x6B5333 },
			{ "planks", 0xA2834F }, { "glass", 0xC0E0F0 }, { "coal_ore", 0x5E5E5E }, { "iron_ore", 0x887A6E },
			{ "gold_ore", 0x8F8A60 }, { "diamond_ore", 0x5E8C8C }, { "netherrack", 0x6F3534 }, { "obsidian", 0x1B1729 },
			{ "terracotta", 0x985E43 }, { "andesite", 0x888888 }, { "diorite", 0xBDBDBD }, { "granite", 0x956755 },
			{ "tuff", 0x6C6D66 }, { "moss", 0x596E2D }, { "mud", 0x3C393D }, { "short_grass", 0x5E9B3A },
			{ "tall_grass", 0x5E9B3A }, { "fern", 0x4F8A35 }, { "flower", 0xD0C040 }, { "poppy", 0xC02020 },
		};

		uint32_t ColorFor( const std::string &n )
		{
			// longest matching key wins
			size_t best = 0; uint32_t rgb = 0;
			for ( const KnownColor &k : kColors )
			{
				size_t l = strlen( k.key );
				if ( l > best && Contains( n, k.key ) ) { best = l; rgb = k.rgb; }
			}
			if ( best ) return RGB( rgb );
			uint32_t h = 2166136261u;
			for ( char c : n ) { h ^= (uint8_t)c; h *= 16777619u; }
			// muted palette
			uint32_t r = 70 + ( h & 0x7F ), g = 70 + ( ( h >> 8 ) & 0x7F ), b = 70 + ( ( h >> 16 ) & 0x7F );
			return RGB( ( r << 16 ) | ( g << 8 ) | b );
		}

		const char *kCrossWords[] = {
			"short_grass", "tall_grass", "fern", "dandelion", "poppy", "orchid", "allium", "azure_bluet", "tulip",
			"oxeye_daisy", "cornflower", "lily_of_the_valley", "wither_rose", "sunflower", "lilac", "rose_bush", "peony",
			"sapling", "dead_bush", "sugar_cane", "wheat", "carrots", "potatoes", "beetroots", "vine", "seagrass", "kelp",
			"bush", "roots", "sprouts", "torch", "_flower", "brown_mushroom", "red_mushroom", "crimson_fungus",
			"warped_fungus", "cobweb", "sweet_berry", "glow_lichen", "hanging_roots", "spore_blossom", "pink_petals",
			"leaf_litter", "wildflowers", "firefly_bush", "cactus_flower", "shrub", "pitcher", "torchflower",
			"_sign", "banner", "rail", "lever", "tripwire", "redstone_wire", "ladder", "_button", "pressure_plate",
			"nether_wart", "eyeblossom", "pale_hanging_moss", "big_dripleaf_stem", "small_dripleaf", "bamboo_sapling",
		};
	}

	BlockRegistry::BlockRegistry()
	{
		BlockDef air;
		air.name = "minecraft:air";
		air.shape = SHAPE_AIR; air.collides = false; air.opaque = false;
		m_defs.push_back( air );
		m_ids[ air.name ] = 0;
		m_ids[ "minecraft:cave_air" ] = 0;
		m_ids[ "minecraft:void_air" ] = 0;
	}

	void BlockRegistry::ApplyHeuristics( BlockDef &d ) const
	{
		const std::string n = StripNamespace( d.name );
		d.color = ColorFor( n );
		d.coll.assign( 1, Box::Unit() );
		d.boxes.clear();

		if ( n == "air" || n == "cave_air" || n == "void_air" || n == "light" || n == "barrier" || n == "structure_void" )
		{
			d.shape = SHAPE_AIR; d.opaque = false;
			d.collides = ( n == "barrier" );
			if ( !d.collides ) d.coll.clear();
			return;
		}
		if ( n == "water" || n == "bubble_column" || n == "lava" )
		{
			d.shape = SHAPE_LIQUID; d.collides = false; d.opaque = false; d.cullSame = true; d.coll.clear(); d.lava = n == "lava";
			d.translucent = ( n != "lava" ); d.emissive = ( n == "lava" );
			d.tint = n == "lava" ? TINT_NONE : TINT_WATER;
			return;
		}
		for ( const char *w : kCrossWords )
		{
			if ( Contains( n, w ) && !Contains( n, "_block" ) && !EndsWith( n, "_wall_sign" ) )
			{
				d.shape = SHAPE_CROSS; d.collides = ( n == "ladder" ); d.opaque = false; d.coll.clear();
				d.climbable = n == "ladder" || Contains( n, "vine" );
				if ( n == "ladder" ) { Box b = { { 0, 0, 13 / 16.f }, { 1, 1, 1 } }; d.coll.assign( 1, b ); }
				if ( n == "short_grass" || n == "tall_grass" || n == "fern" || n == "large_fern" || n == "vine" ) d.tint = TINT_GRASS;
				d.emissive = Contains( n, "torch" );
				return;
			}
		}
		if ( Contains( n, "glass" ) || n == "ice" || Contains( n, "slime_block" ) || Contains( n, "honey_block" ) )
		{
			d.opaque = false; d.cullSame = true;
			d.translucent = Contains( n, "stained" ) || n == "ice" || Contains( n, "slime" ) || Contains( n, "honey" );
			if ( Contains( n, "pane" ) ) { d.SetSingleBox( 7 / 16.f, 0, 7 / 16.f, 9 / 16.f, 1, 9 / 16.f ); }
			return;
		}
		if ( Contains( n, "leaves" ) )
		{
			d.opaque = false; d.cullSame = true;
			d.tint = Contains( n, "spruce" ) ? TINT_SPRUCE : Contains( n, "birch" ) ? TINT_BIRCH : TINT_FOLIAGE;
			if ( Contains( n, "cherry" ) || Contains( n, "azalea" ) || Contains( n, "pale_oak" ) ) d.tint = TINT_NONE;
			return;
		}
		if ( EndsWith( n, "_slab" ) )
		{
			d.opaque = false; d.SetSingleBox( 0, 0, 0, 1, 0.5f, 1 );
			return;
		}
		if ( Contains( n, "carpet" ) || n == "moss_carpet" )
		{
			d.opaque = false; d.SetSingleBox( 0, 0, 0, 1, 1 / 16.f, 1 );
			return;
		}
		if ( n == "snow" )
		{
			d.opaque = false; d.SetSingleBox( 0, 0, 0, 1, 2 / 16.f, 1 );
			return;
		}
		if ( Contains( n, "fence" ) && !Contains( n, "gate" ) )
		{
			d.opaque = false; d.SetSingleBox( 6 / 16.f, 0, 6 / 16.f, 10 / 16.f, 1, 10 / 16.f );
			return;
		}
		if ( EndsWith( n, "_wall" ) )
		{
			d.opaque = false; d.SetSingleBox( 4 / 16.f, 0, 4 / 16.f, 12 / 16.f, 1, 12 / 16.f );
			return;
		}
		if ( n == "farmland" || n == "dirt_path" )
		{
			d.opaque = false; d.SetSingleBox( 0, 0, 0, 1, 15 / 16.f, 1 );
			return;
		}
		if ( Contains( n, "door" ) || Contains( n, "stairs" ) || Contains( n, "chest" ) || Contains( n, "bed" ) ||
			 Contains( n, "lantern" ) || Contains( n, "anvil" ) || Contains( n, "cauldron" ) || Contains( n, "hopper" ) ||
			 Contains( n, "head" ) || Contains( n, "skull" ) || Contains( n, "pot" ) || Contains( n, "candle" ) ||
			 Contains( n, "cake" ) || Contains( n, "bell" ) || Contains( n, "chain" ) || Contains( n, "bars" ) ||
			 Contains( n, "campfire" ) || Contains( n, "lectern" ) || Contains( n, "enchanting" ) || Contains( n, "shelf" ) )
		{
			// Rendered as full cubes but don't hide neighbours (approximation).
			d.opaque = false;
		}
		if ( n == "grass_block" ) { d.tint = TINT_GRASS; d.tintMask = 1 << FACE_UP; }
		d.climbable = d.climbable || n == "scaffolding" || Contains( n, "vines" );
		d.emissive = n == "glowstone" || n == "sea_lantern" || n == "shroomlight" || Contains( n, "froglight" ) ||
					 n == "jack_o_lantern" || n == "magma_block" || n == "redstone_lamp" || n == "beacon";
	}

	int BlockRegistry::LoadDefinitions( const std::string &text )
	{
		++m_generation;
		std::istringstream in( text );
		std::string line;
		int count = 0;
		while ( std::getline( in, line ) )
		{
			size_t hash = line.find( '#' );
			if ( hash != std::string::npos ) line = line.substr( 0, hash );
			std::istringstream ls( line );
			std::string kind;
			if ( !( ls >> kind ) ) continue;
			if ( kind == "atlas" )
			{
				ls >> m_tilesPerRow >> m_rows;
				continue;
			}
			if ( kind != "block" ) continue;

			BlockDef d;
			std::string shape;
			ls >> d.name >> shape;
			if ( d.name.empty() ) continue;
			ApplyHeuristics( d );   // sensible defaults (flags) that the file may override
			for ( int f = 0; f < 6; ++f ) { int t = -1; ls >> t; d.tiles[f] = (int16_t)t; }
			if ( shape == "cube" ) { if ( d.shape == SHAPE_CROSS || d.shape == SHAPE_LIQUID || d.shape == SHAPE_AIR ) { d.shape = SHAPE_CUBE; d.coll.assign( 1, Box::Unit() ); d.collides = true; } }
			else if ( shape == "box" ) d.shape = SHAPE_BOXES;
			else if ( shape == "cross" ) { d.shape = SHAPE_CROSS; d.collides = false; d.opaque = false; d.coll.clear(); }
			else if ( shape == "liquid" ) { d.shape = SHAPE_LIQUID; d.collides = false; d.opaque = false; d.coll.clear(); }
			else if ( shape == "air" ) { d.shape = SHAPE_AIR; d.collides = false; d.opaque = false; d.coll.clear(); }

			std::string kv;
			while ( ls >> kv )
			{
				size_t eq = kv.find( '=' );
				if ( eq == std::string::npos ) continue;
				std::string k = kv.substr( 0, eq ), v = kv.substr( eq + 1 );
				if ( k == "tint" )
				{
					d.tint = v == "grass" ? TINT_GRASS : v == "foliage" ? TINT_FOLIAGE : v == "water" ? TINT_WATER :
							 v == "spruce" ? TINT_SPRUCE : v == "birch" ? TINT_BIRCH : v == "dry_foliage" ? TINT_DRY_FOLIAGE :
							 v == "redstone" ? TINT_REDSTONE : v == "lily" ? TINT_LILY : v == "stem" ? TINT_STEM : TINT_NONE;
				}
				else if ( k == "tintmask" ) d.tintMask = (uint8_t)atoi( v.c_str() );
				else if ( k == "opaque" ) d.opaque = v != "0";
				else if ( k == "collide" ) { d.collides = v != "0"; if ( !d.collides ) d.coll.clear(); else if ( d.coll.empty() ) d.coll.assign( 1, Box::Unit() ); }
				else if ( k == "cullsame" ) d.cullSame = v != "0";
				else if ( k == "translucent" ) d.translucent = v != "0";
				else if ( k == "emissive" ) d.emissive = v != "0";
				else if ( k == "color" ) d.color = RGB( (uint32_t)strtoul( v.c_str(), nullptr, 16 ) );
				else if ( k == "box" )
				{
					float b[6] = { 0, 0, 0, 16, 16, 16 };
					sscanf( v.c_str(), "%f,%f,%f,%f,%f,%f", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5] );
					d.SetSingleBox( b[0] / 16.f, b[1] / 16.f, b[2] / 16.f, b[3] / 16.f, b[4] / 16.f, b[5] / 16.f );
				}
			}
			if ( d.shape == SHAPE_BOXES && d.boxes.empty() ) d.shape = SHAPE_CUBE;
			if ( d.shape == SHAPE_BOXES ) d.opaque = false;
			m_loaded[ d.name ] = d;
			// Update already interned blocks
			auto it = m_ids.find( d.name );
			if ( it != m_ids.end() && it->second != 0 ) m_defs[ it->second ] = d;
			++count;
		}
		return count;
	}

	BlockDef BlockRegistry::BaseDef( const std::string &name ) const
	{
		auto lit = m_loaded.find( name );
		if ( lit != m_loaded.end() ) return lit->second;
		BlockDef d;
		d.name = name;
		ApplyHeuristics( d );
		return d;
	}

	uint16_t BlockRegistry::Intern( const std::string &rawName )
	{
		std::string name = rawName.find( ':' ) == std::string::npos ? "minecraft:" + rawName : rawName;
		auto it = m_ids.find( name );
		if ( it != m_ids.end() ) return it->second;
		if ( m_defs.size() >= 65535 ) return 0;
		++m_generation;
		uint16_t id = (uint16_t)m_defs.size();
		m_defs.push_back( BaseDef( name ) );
		m_ids[ name ] = id;
		return id;
	}

	uint16_t BlockRegistry::InternSound( const std::string &name )
	{
		if ( name.empty() ) return 0;
		auto it = m_soundIds.find( name );
		if ( it != m_soundIds.end() ) return it->second;
		if ( m_soundNames.size() >= 65535 ) return 0;
		uint16_t id = (uint16_t)m_soundNames.size();
		m_soundNames.push_back( name );
		m_soundIds[name] = id;
		return id;
	}

	uint16_t BlockRegistry::DefineState( int32_t stateId, const std::string &rawName, const std::string &props, uint8_t flags, uint8_t light,
										 const std::vector<Box> &coll, const std::vector<Box> &shape, const std::string *sounds )
	{
		std::string name = rawName.find( ':' ) == std::string::npos ? "minecraft:" + rawName : rawName;
		++m_generation;
		BlockDef d = BaseDef( name );
		if ( shape.empty() && coll.empty() && !( flags & ( STATE_LIQUID | STATE_COLLIDES ) ) && d.shape == SHAPE_AIR )
		{
			m_states[stateId] = AIR;   // air, cave_air, void_air, light...: share the AIR id
			return AIR;
		}
		d.props = props;
		d.lightEmission = light;
		if ( sounds )
			for ( int i = 0; i < BlockDef::SND_COUNT; ++i ) d.sounds[i] = InternSound( sounds[i] );
		d.climbable = d.climbable || ( flags & STATE_CLIMBABLE );
		d.waterlogged = ( flags & STATE_WATERLOGGED ) != 0;
		d.collides = ( flags & STATE_COLLIDES ) && !coll.empty();
		d.coll = d.collides ? coll : std::vector<Box>();

		if ( flags & STATE_LIQUID )
		{
			d.shape = SHAPE_LIQUID;
			d.lava = ( flags & STATE_LAVA ) != 0;
			d.translucent = !d.lava;
			d.emissive = d.lava;
			d.opaque = false;
			d.cullSame = true;
			d.tint = d.lava ? TINT_NONE : TINT_WATER;
			int level = 0;
			size_t p = props.find( "level=" );
			if ( p != std::string::npos ) level = atoi( props.c_str() + p + 6 );
			d.liquidHeight = level == 0 ? 8.f / 9.f : level >= 8 ? 1.f : ( 8 - level ) / 9.f;
		}
		else if ( d.shape == SHAPE_CROSS )
		{
			d.boxes = shape;   // picking uses the outline
		}
		else if ( shape.empty() )
		{
			d.shape = SHAPE_AIR;
			d.opaque = false;
		}
		else if ( shape.size() == 1 && shape[0].IsFull() )
		{
			if ( d.shape == SHAPE_AIR || d.shape == SHAPE_BOXES ) d.shape = SHAPE_CUBE;
			d.boxes.clear();
		}
		else
		{
			d.shape = SHAPE_BOXES;
			d.boxes = shape;
		}
		const bool fullShape = shape.size() == 1 && shape[0].IsFull();
		d.opaque = ( flags & STATE_OPAQUE_CUBE ) && fullShape && d.shape != SHAPE_LIQUID && !d.cullSame && !d.translucent;
		d.emissive = d.emissive || light >= 10;

		// Real Minecraft models (mc_models.txt), when available, replace cube/box/cross drawing
		if ( d.shape != SHAPE_LIQUID && d.shape != SHAPE_AIR && m_models )
		{
			d.models = ModelsFor( name, props );
			if ( !d.models.empty() )
			{
				d.shape = SHAPE_MODEL;
				d.boxes = shape;   // picking / outline
			}
		}

		auto it = m_states.find( stateId );
		uint16_t id;
		if ( it != m_states.end() ) { id = it->second; m_defs[id] = d; }
		else
		{
			if ( m_defs.size() >= 65535 ) return 0;
			id = (uint16_t)m_defs.size();
			m_defs.push_back( d );
			m_states[stateId] = id;
		}
		return id;
	}

	int BlockRegistry::LoadModels( const std::string &text )
	{
		++m_generation;
		auto models = std::make_shared<std::vector<BlockModel>>();
		auto rules = std::make_shared<std::unordered_map<std::string, BlockRules>>();
		std::istringstream in( text );
		std::string line;
		BlockModel *cur = nullptr;
		while ( std::getline( in, line ) )
		{
			if ( line.empty() || line[0] == '#' ) continue;
			const char *c = line.c_str();
			if ( c[0] == 'q' && c[1] == ' ' && cur )
			{
				ModelQuad q;
				int tile = -1, cull = -1, face = 0, tint = 0, shade = 1, used = 0;
				if ( sscanf( c + 2, "%d %d %d %d %d%n", &tile, &cull, &face, &tint, &shade, &used ) < 5 ) continue;
				const char *p = c + 2 + used;
				bool ok = true;
				for ( int k = 0; k < 4 && ok; ++k )
				{
					int n = 0;
					ok = sscanf( p, "%f %f %f %f %f%n", &q.pos[k][0], &q.pos[k][1], &q.pos[k][2], &q.uv[k][0], &q.uv[k][1], &n ) == 5;
					p += n;
				}
				if ( !ok ) continue;
				q.tile = (int16_t)tile; q.cull = (int8_t)( cull >= 0 && cull < 6 ? cull : -1 ); q.face = (int8_t)( face >= 0 && face < 6 ? face : 1 );
				q.tint = tint ? 1 : 0; q.shade = shade ? 1 : 0;
				cur->quads.push_back( q );
				continue;
			}
			std::istringstream ls( line );
			std::string kind;
			ls >> kind;
			if ( kind == "models" ) { size_t n = 0; ls >> n; models->reserve( n ); }
			else if ( kind == "m" )
			{
				size_t id = 0;
				ls >> id;
				if ( id >= models->size() ) models->resize( id + 1 );
				cur = &( *models )[id];
			}
			else if ( kind == "v" || kind == "p" )
			{
				std::string name, cond;
				uint32_t model = 0;
				if ( !( ls >> name >> cond >> model ) ) continue;
				BlockRules &br = ( *rules )[name];
				br.multipart = kind == "p";
				Rule r;
				r.model = model;
				if ( cond != "-" )
				{
					std::istringstream alts( cond );
					std::string alt;
					while ( std::getline( alts, alt, ';' ) )
					{
						std::vector<RuleTerm> terms;
						std::istringstream ts( alt );
						std::string term;
						while ( std::getline( ts, term, ',' ) )
						{
							size_t eq = term.find( '=' );
							if ( eq == std::string::npos ) continue;
							RuleTerm t;
							t.key = term.substr( 0, eq );
							std::string vals = term.substr( eq + 1 );
							if ( !vals.empty() && vals[0] == '!' ) { t.negate = true; vals = vals.substr( 1 ); }
							std::istringstream vs( vals );
							std::string v;
							while ( std::getline( vs, v, '|' ) ) t.values.push_back( v );
							terms.push_back( t );
						}
						r.any.push_back( terms );
					}
				}
				br.rules.push_back( r );
			}
		}
		m_models = models;
		m_rules = rules;
		return (int)models->size();
	}

	std::vector<uint32_t> BlockRegistry::ModelsFor( const std::string &name, const std::string &props ) const
	{
		std::vector<uint32_t> out;
		if ( !m_rules || !m_models ) return out;
		auto it = m_rules->find( name );
		if ( it == m_rules->end() ) return out;
		// parse "a=b,c=d"
		std::vector<std::pair<std::string, std::string>> kv;
		size_t start = 0;
		while ( start < props.size() )
		{
			size_t end = props.find( ',', start );
			if ( end == std::string::npos ) end = props.size();
			size_t eq = props.find( '=', start );
			if ( eq != std::string::npos && eq < end ) kv.push_back( std::make_pair( props.substr( start, eq - start ), props.substr( eq + 1, end - eq - 1 ) ) );
			start = end + 1;
		}
		auto value = [&]( const std::string &k ) -> const std::string * {
			for ( const auto &p : kv ) if ( p.first == k ) return &p.second;
			return nullptr;
		};
		auto termOk = [&]( const RuleTerm &t ) {
			const std::string *v = value( t.key );
			if ( !v ) return false;
			bool in = std::find( t.values.begin(), t.values.end(), *v ) != t.values.end();
			return t.negate ? !in : in;
		};
		auto ruleOk = [&]( const Rule &r ) {
			if ( r.any.empty() ) return true;
			for ( const auto &terms : r.any )
			{
				bool all = true;
				for ( const RuleTerm &t : terms ) if ( !termOk( t ) ) { all = false; break; }
				if ( all ) return true;
			}
			return false;
		};
		const BlockRules &br = it->second;
		for ( const Rule &r : br.rules )
		{
			if ( !ruleOk( r ) || r.model >= m_models->size() ) continue;
			out.push_back( r.model );
			if ( !br.multipart ) break;   // variants: first match wins
		}
		return out;
	}

	uint16_t BlockRegistry::ForState( int32_t stateId )
	{
		auto it = m_states.find( stateId );
		if ( it != m_states.end() ) return it->second;
		if ( !m_unknown ) { m_unknown = Intern( "tf2mc:unknown" ); }
		return m_unknown;
	}

	//==================================================================
	// VoxelWorld
	//==================================================================
	VoxelWorld::VoxelWorld() {}

	void VoxelWorld::MarkRemoved( const SectionKey &k )
	{
		for ( int c = 0; c < CH_COUNT; ++c ) { m_dirty[c].erase( k ); m_removed[c].insert( k ); }
	}

	void VoxelWorld::Reset( int minSectionY, int sectionCount )
	{
		for ( auto &c : m_columns )
		{
			int cx = (int)( c.first >> 32 ), cz = (int)(int32_t)( c.first & 0xFFFFFFFF );
			for ( size_t i = 0; i < c.second.sections.size(); ++i )
				if ( c.second.sections[i] )
					MarkRemoved( SectionKey{ cx, m_minSection + (int)i, cz } );
		}
		m_minSection = minSectionY;
		m_sectionCount = std::max( 1, std::min( sectionCount, 256 ) );
		m_columns.clear();
		for ( int c = 0; c < CH_COUNT; ++c ) m_dirty[c].clear();
	}

	bool VoxelWorld::IsColumnLoaded( int cx, int cz ) const
	{
		return m_columns.find( ColKey( cx, cz ) ) != m_columns.end();
	}

	void VoxelWorld::CopyNeighborhood( const VoxelWorld &src, int cx, int sy, int cz, const BlockRegistry *reg )
	{
		m_columns.clear();
		m_minSection = src.m_minSection;
		m_sectionCount = src.m_sectionCount;
		m_regView = reg;
		for ( int dx = -1; dx <= 1; ++dx )
			for ( int dz = -1; dz <= 1; ++dz )
			{
				auto it = src.m_columns.find( ColKey( cx + dx, cz + dz ) );
				if ( it == src.m_columns.end() ) continue;
				Column &col = m_columns[ ColKey( cx + dx, cz + dz ) ];
				col.sections.resize( it->second.sections.size() );
				for ( int dy = -1; dy <= 1; ++dy )
				{
					int i = sy + dy - m_minSection;
					if ( i < 0 || i >= (int)it->second.sections.size() || !it->second.sections[i] ) continue;
					col.sections[i].reset( new Section( *it->second.sections[i] ) );
				}
			}
	}

	const Section *VoxelWorld::GetSection( int cx, int sy, int cz ) const
	{
		auto it = m_columns.find( ColKey( cx, cz ) );
		if ( it == m_columns.end() ) return nullptr;
		int i = sy - m_minSection;
		if ( i < 0 || i >= (int)it->second.sections.size() ) return nullptr;
		return it->second.sections[i].get();
	}

	Section *VoxelWorld::EnsureSection( int cx, int sy, int cz )
	{
		int i = sy - m_minSection;
		if ( i < 0 || i >= m_sectionCount ) return nullptr;
		Column &col = m_columns[ ColKey( cx, cz ) ];
		if ( (int)col.sections.size() < m_sectionCount ) col.sections.resize( m_sectionCount );
		if ( !col.sections[i] ) col.sections[i].reset( new Section() );
		return col.sections[i].get();
	}

	uint16_t VoxelWorld::Get( int x, int y, int z ) const
	{
		const Section *s = GetSection( FloorDiv16( x ), FloorDiv16( y ), FloorDiv16( z ) );
		if ( !s ) return BlockRegistry::AIR;
		return s->blocks[ Section::Index( x & 15, y & 15, z & 15 ) ];
	}

	int VoxelWorld::SkyLight( int x, int y, int z ) const
	{
		const Section *s = GetSection( FloorDiv16( x ), FloorDiv16( y ), FloorDiv16( z ) );
		if ( !s || !s->hasSky ) return 15;
		return Section::Nibble( s->sky, Section::Index( x & 15, y & 15, z & 15 ) );
	}

	int VoxelWorld::BlockLight( int x, int y, int z ) const
	{
		const Section *s = GetSection( FloorDiv16( x ), FloorDiv16( y ), FloorDiv16( z ) );
		if ( !s || !s->hasBlk ) return 0;
		return Section::Nibble( s->blk, Section::Index( x & 15, y & 15, z & 15 ) );
	}

	bool VoxelWorld::IsWaterAt( double x, double y, double z ) const
	{
		int bx = FloorI( x ), by = FloorI( y ), bz = FloorI( z );
		const BlockDef &d = GetDef( bx, by, bz );
		if ( d.waterlogged ) return true;
		if ( d.shape != SHAPE_LIQUID || d.lava ) return false;
		return ( y - by ) <= d.liquidHeight + 1e-6 || GetDef( bx, by + 1, bz ).shape == SHAPE_LIQUID;
	}

	bool VoxelWorld::IsLavaAt( double x, double y, double z ) const
	{
		const BlockDef &d = GetDef( FloorI( x ), FloorI( y ), FloorI( z ) );
		return d.shape == SHAPE_LIQUID && d.lava;
	}

	void VoxelWorld::MarkDirty( int cx, int sy, int cz, bool force, unsigned channels )
	{
		if ( sy < m_minSection || sy >= m_minSection + m_sectionCount ) return;
		if ( !IsColumnLoaded( cx, cz ) ) return;
		if ( !force && !GetSection( cx, sy, cz ) ) return;   // empty neighbours have no mesh to rebuild
		for ( int c = 0; c < CH_COUNT; ++c )
			if ( channels & ( 1u << c ) ) m_dirty[c].insert( SectionKey{ cx, sy, cz } );
	}

	void VoxelWorld::SetBlock( int x, int y, int z, uint16_t id )
	{
		int cx = FloorDiv16( x ), sy = FloorDiv16( y ), cz = FloorDiv16( z );
		auto it = m_columns.find( ColKey( cx, cz ) );
		if ( it == m_columns.end() ) return;   // ignore edits in unloaded columns
		int i = sy - m_minSection;
		if ( i < 0 || i >= m_sectionCount ) return;
		auto &secs = it->second.sections;
		if ( (int)secs.size() < m_sectionCount ) secs.resize( m_sectionCount );
		if ( !secs[i] )
		{
			if ( id == BlockRegistry::AIR ) return;
			secs[i].reset( new Section() );
		}
		Section &s = *secs[i];
		uint16_t &slot = s.blocks[ Section::Index( x & 15, y & 15, z & 15 ) ];
		if ( slot == id ) return;
		if ( slot == BlockRegistry::AIR ) s.nonAir++;
		if ( id == BlockRegistry::AIR ) s.nonAir--;
		slot = id;

		MarkDirty( cx, sy, cz, true );
		int lx = x & 15, ly = y & 15, lz = z & 15;
		if ( lx == 0 ) MarkDirty( cx - 1, sy, cz );
		if ( lx == 15 ) MarkDirty( cx + 1, sy, cz );
		if ( ly == 0 ) MarkDirty( cx, sy - 1, cz );
		if ( ly == 15 ) MarkDirty( cx, sy + 1, cz );
		if ( lz == 0 ) MarkDirty( cx, sy, cz - 1 );
		if ( lz == 15 ) MarkDirty( cx, sy, cz + 1 );
		// Fences/walls reach into the block above: keep its collision fresh too
		if ( ly == 15 ) MarkDirty( cx, sy + 1, cz, false, 1u << CH_PHYSICS );
	}

	void VoxelWorld::SetSection( int cx, int sy, int cz, const std::vector<uint16_t> &palette, const uint16_t *indices )
	{
		Section *s = EnsureSection( cx, sy, cz );
		if ( !s ) return;
		int nonAir = 0;
		for ( int k = 0; k < 4096; ++k )
		{
			uint16_t pi = indices[k];
			uint16_t id = pi < palette.size() ? palette[pi] : 0;
			s->blocks[k] = id;
			if ( id != BlockRegistry::AIR ) nonAir++;
		}
		s->nonAir = nonAir;

		for ( int c = 0; c < CH_COUNT; ++c ) m_removed[c].erase( SectionKey{ cx, sy, cz } );
		MarkDirty( cx, sy, cz, true );
		MarkDirty( cx - 1, sy, cz ); MarkDirty( cx + 1, sy, cz );
		MarkDirty( cx, sy - 1, cz ); MarkDirty( cx, sy + 1, cz );
		MarkDirty( cx, sy, cz - 1 ); MarkDirty( cx, sy, cz + 1 );
	}

	void VoxelWorld::SetLight( int cx, int sy, int cz, const uint8_t *sky, const uint8_t *blk )
	{
		if ( !IsColumnLoaded( cx, cz ) ) return;
		Section *s = EnsureSection( cx, sy, cz );
		if ( !s ) return;
		s->hasSky = sky != nullptr;
		s->hasBlk = blk != nullptr;
		if ( sky ) memcpy( s->sky, sky, 2048 ); else memset( s->sky, 0xFF, 2048 );
		if ( blk ) memcpy( s->blk, blk, 2048 ); else memset( s->blk, 0, 2048 );
		// Light only affects rendering (smooth lighting samples the neighbours too)
		const unsigned R = 1u << CH_RENDER;
		MarkDirty( cx, sy, cz, true, R );
		MarkDirty( cx - 1, sy, cz, false, R ); MarkDirty( cx + 1, sy, cz, false, R );
		MarkDirty( cx, sy - 1, cz, false, R ); MarkDirty( cx, sy + 1, cz, false, R );
		MarkDirty( cx, sy, cz - 1, false, R ); MarkDirty( cx, sy, cz + 1, false, R );
	}

	void VoxelWorld::UnloadColumn( int cx, int cz )
	{
		auto it = m_columns.find( ColKey( cx, cz ) );
		if ( it == m_columns.end() ) return;
		for ( int i = 0; i < m_sectionCount; ++i ) MarkRemoved( SectionKey{ cx, m_minSection + i, cz } );
		m_columns.erase( it );
		for ( int i = 0; i < m_sectionCount; ++i )
		{
			MarkDirty( cx - 1, m_minSection + i, cz ); MarkDirty( cx + 1, m_minSection + i, cz );
			MarkDirty( cx, m_minSection + i, cz - 1 ); MarkDirty( cx, m_minSection + i, cz + 1 );
		}
	}

	static void ReadBoxes( Reader &r, std::vector<Box> &out )
	{
		int n = r.u8();
		out.clear();
		for ( int i = 0; i < n && r.ok(); ++i )
		{
			Box b;
			b.mn[0] = r.f32(); b.mn[1] = r.f32(); b.mn[2] = r.f32();
			b.mx[0] = r.f32(); b.mx[1] = r.f32(); b.mx[2] = r.f32();
			out.push_back( b );
		}
	}

	bool VoxelWorld::ApplyStateDefsPacket( Reader &r )
	{
		int count = r.u16();
		std::vector<Box> coll, shape;
		for ( int i = 0; i < count && r.ok(); ++i )
		{
			int32_t id = r.i32();
			std::string name = r.str(), props = r.str();
			uint8_t flags = r.u8(), light = r.u8();
			ReadBoxes( r, coll );
			ReadBoxes( r, shape );
			std::string sounds[BlockDef::SND_COUNT];
			for ( int k = 0; k < BlockDef::SND_COUNT; ++k ) sounds[k] = r.str();   // v3
			if ( !r.ok() ) return false;
			m_registry.DefineState( id, name, props, flags, light, coll, shape, sounds );
		}
		return r.ok();
	}

	bool VoxelWorld::ReadLight( Reader &r, uint8_t *sky, uint8_t *blk, bool &hasSky, bool &hasBlk )
	{
		uint8_t lf = r.u8();
		hasSky = ( lf & 1 ) != 0;
		hasBlk = ( lf & 2 ) != 0;
		if ( hasSky ) { const uint8_t *p = r.bytes( 2048 ); if ( !p ) return false; memcpy( sky, p, 2048 ); }
		if ( hasBlk ) { const uint8_t *p = r.bytes( 2048 ); if ( !p ) return false; memcpy( blk, p, 2048 ); }
		return r.ok();
	}

	bool VoxelWorld::ApplySectionPacket( Reader &r )
	{
		int cx = r.i32(), sy = r.i32(), cz = r.i32();
		uint16_t paletteCount = r.u16();
		if ( !r.ok() || paletteCount == 0 ) return false;
		std::vector<uint16_t> palette( paletteCount );
		for ( uint16_t i = 0; i < paletteCount; ++i ) palette[i] = m_registry.ForState( r.i32() );
		static uint16_t idx[4096];
		if ( paletteCount <= 256 )
		{
			const uint8_t *b = r.bytes( 4096 );
			if ( !b ) return false;
			for ( int i = 0; i < 4096; ++i ) idx[i] = b[i];
		}
		else
		{
			const uint8_t *b = r.bytes( 8192 );
			if ( !b ) return false;
			memcpy( idx, b, 8192 );
		}
		static uint8_t sky[2048], blk[2048];
		bool hasSky = false, hasBlk = false;
		if ( r.remaining() > 0 && !ReadLight( r, sky, blk, hasSky, hasBlk ) ) return false;
		if ( !r.ok() ) return false;
		if ( !IsColumnLoaded( cx, cz ) ) m_columns[ ColKey( cx, cz ) ].sections.resize( m_sectionCount );
		SetSection( cx, sy, cz, palette, idx );
		if ( Section *s = EnsureSection( cx, sy, cz ) )
		{
			s->hasSky = hasSky; s->hasBlk = hasBlk;
			if ( hasSky ) memcpy( s->sky, sky, 2048 );
			if ( hasBlk ) memcpy( s->blk, blk, 2048 );
		}
		return true;
	}

	bool VoxelWorld::ApplyLightPacket( Reader &r )
	{
		int cx = r.i32(), sy = r.i32(), cz = r.i32();
		static uint8_t sky[2048], blk[2048];
		bool hasSky = false, hasBlk = false;
		if ( !ReadLight( r, sky, blk, hasSky, hasBlk ) ) return false;
		SetLight( cx, sy, cz, hasSky ? sky : nullptr, hasBlk ? blk : nullptr );
		return true;
	}

	bool VoxelWorld::ApplyBlockSetPacket( Reader &r, BlockPos *outPos, uint16_t *outOld, uint16_t *outNew )
	{
		int x = r.i32(), y = r.i32(), z = r.i32();
		int32_t state = r.i32();
		if ( !r.ok() ) return false;
		uint16_t old = Get( x, y, z );
		uint16_t id = m_registry.ForState( state );
		SetBlock( x, y, z, id );
		if ( outPos ) *outPos = BlockPos( x, y, z );
		if ( outOld ) *outOld = old;
		if ( outNew ) *outNew = id;
		return true;
	}

	bool VoxelWorld::ApplyUnloadPacket( Reader &r )
	{
		int cx = r.i32(), cz = r.i32();
		if ( !r.ok() ) return false;
		UnloadColumn( cx, cz );
		return true;
	}

	void VoxelWorld::TakeDirty( std::vector<SectionKey> &out, int channel )
	{
		out.insert( out.end(), m_dirty[channel].begin(), m_dirty[channel].end() );
		m_dirty[channel].clear();
	}

	void VoxelWorld::TakeRemoved( std::vector<SectionKey> &out, int channel )
	{
		out.insert( out.end(), m_removed[channel].begin(), m_removed[channel].end() );
		m_removed[channel].clear();
	}

	int VoxelWorld::TopSolidY( int x, int z ) const
	{
		if ( !IsColumnLoaded( FloorDiv16( x ), FloorDiv16( z ) ) ) return MinY() - 1;
		for ( int y = MaxY() - 1; y >= MinY(); --y )
		{
			const BlockDef &d = GetDef( x, y, z );
			if ( d.collides || d.shape == SHAPE_LIQUID ) return y;
		}
		return MinY() - 1;
	}
}
