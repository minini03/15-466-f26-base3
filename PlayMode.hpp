#include "Mode.hpp"

#include "Scene.hpp"
#include "Sound.hpp"

#include <glm/glm.hpp>

#include <array>
#include <vector>
#include <deque>
#include <memory>

struct PlayMode : Mode {
	PlayMode();
	virtual ~PlayMode();

	//functions called by main loop:
	virtual bool handle_event(SDL_Event const &, glm::uvec2 const &window_size) override;
	virtual void update(float elapsed) override;
	virtual void draw(glm::uvec2 const &drawable_size) override;

	//----- game state -----

	//input tracking:
	struct Button {
		uint8_t downs = 0;
		uint8_t pressed = 0;
	} left, right, down, up;

	//local copy of the game scene (so code can change it during gameplay):
	Scene scene;

	// Ghost1/3/4/5: reverse on wall; Ghost2: turn left on wall
	struct Ghost {
		Scene::Transform *transform = nullptr;
		glm::vec3 velocity = glm::vec3(2.0f, 0.0f, 0.0f);
		glm::vec3 home_position = glm::vec3(0.0f);
		glm::vec3 home_velocity = glm::vec3(2.0f, 0.0f, 0.0f);
		bool returning_home = false;
		std::shared_ptr< Sound::PlayingSample > loop;
		bool turn_left_on_wall = false;
	};
	std::array< Ghost, 5 > ghosts;
	static constexpr float MonsterRadius = 0.7f;
	static constexpr float MonsterSoundRadius = 1.2f; // smaller = sharper distance falloff
	static constexpr float MonsterSoundVolume = 4.0f; // louder when close

	Scene::Transform *key = nullptr;
	Scene::Transform *exit_transform = nullptr;
	Scene::Transform *drum = nullptr;
	static constexpr float PickupRadius = 1.0f;
	static constexpr float ShootRange = 20.0f;
	static constexpr float DrumHitRadius = 1.2f;
	bool need_key_message = false;
	float shoot_message_timer = 0.0f;
	float drum_attract_timer = 0.0f; // Ghost3/4 chase drum while > 0
	static constexpr float DrumAttractDuration = 8.0f;
	static constexpr float ShootMessageDuration = 3.0f;

	std::shared_ptr< Sound::PlayingSample > drum_oneshot;

	// true if a forward shot from the player would hit the drum
	bool shot_hits_drum() const;

	//ambient background music (2D):
	std::shared_ptr< Sound::PlayingSample > bgm_loop;

	//camera:
	Scene::Camera *camera = nullptr;
	float camera_height = 2.0f;

	// free movement: hold W/S to walk, A/D to turn
	static constexpr float PlayerSpeed = 4.0f;
	static constexpr float TurnSpeed = glm::radians(120.0f); // rad/sec
	static constexpr float PlayerRadius = 0.35f;

	struct Wall {
		glm::vec2 min = glm::vec2(0.0f);
		glm::vec2 max = glm::vec2(0.0f);
	};
	static std::vector< Wall > walls;

	bool hits_wall(glm::vec2 const &p, float radius = PlayerRadius) const;
	// true if segment player↔ghost intersects any wall AABB
	bool wall_blocks(glm::vec2 const &a, glm::vec2 const &b) const;

	bool won = false;
	bool lost = false;
	bool have_key = false;
	bool restart_requested = false;
	bool showing_intro = true;

};
