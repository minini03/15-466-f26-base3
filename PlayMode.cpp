#include "PlayMode.hpp"

#include "LitColorTextureProgram.hpp"

#include "DrawLines.hpp"
#include "Mesh.hpp"
#include "Load.hpp"
#include "gl_errors.hpp"
#include "data_path.hpp"

#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <limits>
#include <algorithm>

GLuint maze_meshes_for_lit_color_texture_program = 0;
Load< MeshBuffer > maze_meshes(LoadTagDefault, []() -> MeshBuffer const * {
	MeshBuffer const *ret = new MeshBuffer(data_path("maze.pnct"));
	maze_meshes_for_lit_color_texture_program = ret->make_vao_for_program(lit_color_texture_program->program);
	return ret;
});

std::vector< PlayMode::Wall > PlayMode::walls;

Load< Scene > maze_scene(LoadTagDefault, []() -> Scene const * {
	PlayMode::walls.clear();
	return new Scene(data_path("maze.scene"), [&](Scene &scene, Scene::Transform *transform, std::string const &mesh_name){
		Mesh const &mesh = maze_meshes->lookup(mesh_name);

		scene.drawables.emplace_back(transform);
		Scene::Drawable &drawable = scene.drawables.back();

		drawable.pipeline = lit_color_texture_program_pipeline;

		drawable.pipeline.vao = maze_meshes_for_lit_color_texture_program;
		drawable.pipeline.type = mesh.type;
		drawable.pipeline.start = mesh.start;
		drawable.pipeline.count = mesh.count;

		if (transform && transform->name.rfind("Cube", 0) == 0) {
			// save walls position from local mesh + transform
			glm::mat4x3 world_from_local = transform->make_world_from_local();
			glm::vec2 bmin(std::numeric_limits<float>::infinity());
			glm::vec2 bmax(-std::numeric_limits<float>::infinity());
			for (int i = 0; i < 8; ++i) {
				glm::vec3 vertex(
					(i & 1) ? mesh.max.x : mesh.min.x,
					(i & 2) ? mesh.max.y : mesh.min.y,
					(i & 4) ? mesh.max.z : mesh.min.z
				);
				glm::vec3 world = world_from_local[0] * vertex.x
					+ world_from_local[1] * vertex.y
					+ world_from_local[2] * vertex.z
					+ world_from_local[3];
				bmin = glm::min(bmin, glm::vec2(world));
				bmax = glm::max(bmax, glm::vec2(world));
			}
			PlayMode::walls.push_back(PlayMode::Wall{bmin, bmax});
		}

	});
});

Load< Sound::Sample > ghost_sample(LoadTagDefault, []() -> Sound::Sample const * {
	return new Sound::Sample(data_path("ghost.wav"));
});

Load< Sound::Sample > bgm_sample(LoadTagDefault, []() -> Sound::Sample const * {
	return new Sound::Sample(data_path("bgm.wav"));
});

Load< Sound::Sample > drum_sample(LoadTagDefault, []() -> Sound::Sample const * {
	return new Sound::Sample(data_path("drum.wav"));
});


PlayMode::PlayMode() : scene(*maze_scene) {
	for (auto &transform : scene.transforms) {
		if (transform.name == "Ghost1") ghosts[0].transform = &transform;
		else if (transform.name == "Ghost2") ghosts[1].transform = &transform;
		else if (transform.name == "Ghost3") ghosts[2].transform = &transform;
		else if (transform.name == "Ghost4") ghosts[3].transform = &transform;
		else if (transform.name == "Ghost5") ghosts[4].transform = &transform;
		else if (transform.name == "Key") key = &transform;
		else if (transform.name == "Exit") exit_transform = &transform;
		else if (transform.name == "Drum") drum = &transform;
	}
	// only Ghost2 turns left on walls; 1/3/4/5 reverse like Ghost1
	ghosts[1].turn_left_on_wall = true;
	ghosts[2].velocity = glm::vec3(0.0f, -2.0f, 0.0f); // Ghost3
	ghosts[3].velocity = glm::vec3(0.0f, 2.0f, 0.0f); // Ghost4

	for (auto &c : scene.cameras) {
		if (c.transform && c.transform->name == "Camera") {
			camera = &c;
			break;
		}
	}
	if (camera == nullptr) throw std::runtime_error("Camera not found.");
	if (key == nullptr) throw std::runtime_error("Key not found.");
	if (exit_transform == nullptr) throw std::runtime_error("Exit not found.");
	if (drum == nullptr) throw std::runtime_error("Drum not found.");
	camera_height = camera->transform->position.z;
	camera->fovy = glm::radians(90.0f);

	for (auto &g : ghosts) {
		if (!g.transform) continue;
		g.home_position = g.transform->position;
		g.home_velocity = g.velocity;
		g.loop = Sound::loop_3D(*ghost_sample, MonsterSoundVolume, g.transform->make_world_from_local()[3], MonsterSoundRadius);
	}
	bgm_loop = Sound::loop(*bgm_sample, 0.5f);
}

PlayMode::~PlayMode() {
}

bool PlayMode::hits_wall(glm::vec2 const &p, float radius) const {
	// check if hit walls
	for (auto const &w : walls) {
		if (p.x + radius > w.min.x && p.x - radius < w.max.x
		 && p.y + radius > w.min.y && p.y - radius < w.max.y) {
			return true;
		}
	}
	return false;
}

bool PlayMode::wall_blocks(glm::vec2 const &a, glm::vec2 const &b) const {
	// Liang-Barsky: does open segment (a,b) hit any wall AABB?
	glm::vec2 d = b - a;
	for (auto const &w : walls) {
		float t0 = 0.0f;
		float t1 = 1.0f;
		auto clip = [&](float p, float q) {
			if (std::abs(p) < 1e-8f) return q >= 0.0f;
			float r = q / p;
			if (p < 0.0f) {
				if (r > t1) return false;
				if (r > t0) t0 = r;
			} else {
				if (r < t0) return false;
				if (r < t1) t1 = r;
			}
			return true;
		};
		if (!clip(-d.x, a.x - w.min.x)) continue;
		if (!clip( d.x, w.max.x - a.x)) continue;
		if (!clip(-d.y, a.y - w.min.y)) continue;
		if (!clip( d.y, w.max.y - a.y)) continue;
		float hi0 = std::max(t0, 0.0f);
		float hi1 = std::min(t1, 1.0f);
		if (hi1 > hi0 + 1e-4f) return true;
	}
	return false;
}

bool PlayMode::shot_hits_drum() const {
	if (!drum || !camera) return false;
	glm::mat4x3 frame = camera->transform->make_parent_from_local();
	glm::vec2 origin(camera->transform->position);
	glm::vec3 fwd3 = -glm::vec3(frame[2]);
	fwd3.z = 0.0f;
	if (glm::length(fwd3) < 1e-4f) return false;
	glm::vec2 dir = glm::normalize(glm::vec2(fwd3));

	glm::vec2 drum_pos(drum->make_world_from_local()[3]);
	glm::vec2 to = drum_pos - origin;
	float along = glm::dot(to, dir);
	if (along < 0.0f || along > ShootRange) return false;

	glm::vec2 closest = origin + dir * along;
	if (glm::length(closest - drum_pos) > DrumHitRadius) return false;

	// walls between player and drum block the shot
	if (wall_blocks(origin, drum_pos)) return false;
	return true;
}

bool PlayMode::handle_event(SDL_Event const &evt, glm::uvec2 const &window_size) {

	if (evt.type == SDL_EVENT_KEY_DOWN && evt.key.key == SDLK_R) {
		restart_requested = true;
		return true;
	}

	if (showing_intro) {
		if (evt.type == SDL_EVENT_KEY_DOWN && evt.key.key == SDLK_SPACE) {
			showing_intro = false;
			return true;
		}
		return false;
	}

	if (lost || won) return false;

	if (evt.type == SDL_EVENT_KEY_DOWN) {
		if (evt.key.key == SDLK_A) {
			left.downs += 1;
			left.pressed = true;
			return true;
		} else if (evt.key.key == SDLK_D) {
			right.downs += 1;
			right.pressed = true;
			return true;
		} else if (evt.key.key == SDLK_W) {
			up.downs += 1;
			up.pressed = true;
			return true;
		} else if (evt.key.key == SDLK_S) {
			down.downs += 1;
			down.pressed = true;
			return true;
		} else if (evt.key.key == SDLK_SPACE) {
			// shoot forward; ringing the drum if hit
			shoot_message_timer = ShootMessageDuration;
			if (shot_hits_drum()) {
				if (drum_oneshot) drum_oneshot->stop();
				glm::vec3 drum_pos = drum->make_world_from_local()[3];
				drum_oneshot = Sound::play_3D(*drum_sample, 1.0f, drum_pos, 8.0f);
				drum_attract_timer = DrumAttractDuration;
			}
			return true;
		}
	} else if (evt.type == SDL_EVENT_KEY_UP) {
		if (evt.key.key == SDLK_A) {
			left.pressed = false;
			return true;
		} else if (evt.key.key == SDLK_D) {
			right.pressed = false;
			return true;
		} else if (evt.key.key == SDLK_W) {
			up.pressed = false;
			return true;
		} else if (evt.key.key == SDLK_S) {
			down.pressed = false;
			return true;
		}
	}

	return false;
}

void PlayMode::update(float elapsed) {

	if (restart_requested) {
		Mode::set_current(std::make_shared< PlayMode >());
		return;
	}
	if (showing_intro || lost || won) {
		left.downs = right.downs = up.downs = down.downs = 0;
		return;
	}

	need_key_message = false;
	if (shoot_message_timer > 0.0f) {
		shoot_message_timer = std::max(0.0f, shoot_message_timer - elapsed);
	}
	if (drum_attract_timer > 0.0f) {
		drum_attract_timer = std::max(0.0f, drum_attract_timer - elapsed);
	}

	glm::vec2 drum_target(0.0f);
	bool attracting = drum && drum_attract_timer > 0.0f;
	if (attracting) {
		drum_target = glm::vec2(drum->make_world_from_local()[3]);
	}

	// ghosts: 1/3/4/5 reverse on wall; 2 turns left; 3/4 chase drum while attracted
	for (size_t gi = 0; gi < ghosts.size(); ++gi) {
		auto &g = ghosts[gi];
		if (!g.transform) continue;

		bool chase_drum = attracting && (gi == 2 || gi == 3);
		if (chase_drum) {
			glm::vec2 pos(g.transform->position);
			glm::vec2 to = drum_target - pos;
			float speed = std::max(2.0f, glm::length(glm::vec2(g.velocity)));
			if (glm::length(to) > 0.4f) {
				if (std::abs(to.x) >= std::abs(to.y)) {
					g.velocity = glm::vec3(std::copysign(speed, to.x), 0.0f, 0.0f);
				} else {
					g.velocity = glm::vec3(0.0f, std::copysign(speed, to.y), 0.0f);
				}
			}
		}

		glm::vec3 next = g.transform->position + g.velocity * elapsed;
		if (hits_wall(glm::vec2(next), MonsterRadius)) {
			if (chase_drum) {
				// try the other axis toward the drum, else reverse
				float speed = std::max(2.0f, glm::length(glm::vec2(g.velocity)));
				glm::vec2 pos(g.transform->position);
				glm::vec2 to = drum_target - pos;
				glm::vec3 alt = (std::abs(g.velocity.x) > 0.1f)
					? glm::vec3(0.0f, std::copysign(speed, to.y == 0.0f ? 1.0f : to.y), 0.0f)
					: glm::vec3(std::copysign(speed, to.x == 0.0f ? 1.0f : to.x), 0.0f, 0.0f);
				glm::vec3 alt_next = g.transform->position + alt * elapsed;
				if (!hits_wall(glm::vec2(alt_next), MonsterRadius)) {
					g.velocity = alt;
					next = alt_next;
				} else {
					g.velocity = -g.velocity;
					next = g.transform->position + g.velocity * elapsed;
				}
			} else if (g.turn_left_on_wall) {
				g.velocity = glm::vec3(-g.velocity.y, g.velocity.x, 0.0f);
				next = g.transform->position + g.velocity * elapsed;
			} else {
				g.velocity = -g.velocity;
				next = g.transform->position + g.velocity * elapsed;
			}
		}
		if (!hits_wall(glm::vec2(next), MonsterRadius)) {
			g.transform->position.x = next.x;
			g.transform->position.y = next.y;
		}
		g.transform->rotation = glm::angleAxis(
			std::atan2(g.velocity.y, g.velocity.x),
			glm::vec3(0.0f, 0.0f, 1.0f)
		);
		if (g.loop) {
			g.loop->set_position(g.transform->make_world_from_local()[3], 1.0f / 60.0f);
		}
	}

	// A/D: smooth turn while held
	if (left.pressed) {
		camera->transform->rotation = glm::normalize(
			glm::angleAxis(TurnSpeed * elapsed, glm::vec3(0.0f, 0.0f, 1.0f)) * camera->transform->rotation
		);
	}
	if (right.pressed) {
		camera->transform->rotation = glm::normalize(
			glm::angleAxis(-TurnSpeed * elapsed, glm::vec3(0.0f, 0.0f, 1.0f)) * camera->transform->rotation
		);
	}

	// W/S: continuous move along facing direction
	glm::mat4x3 frame = camera->transform->make_parent_from_local();
	glm::vec3 forward = -glm::vec3(frame[2]);
	forward.z = 0.0f;
	if (glm::length(forward) > 1e-4f) forward = glm::normalize(forward);

	glm::vec3 move = glm::vec3(0.0f);
	if (up.pressed) move += forward;
	if (down.pressed) move -= forward;
	if (glm::length(move) > 1e-4f) {
		move = glm::normalize(move) * PlayerSpeed * elapsed;
		glm::vec3 pos = camera->transform->position + move;
		pos.z = camera_height;
		if (!hits_wall(glm::vec2(pos))) {
			camera->transform->position = pos;
		} else {
			// slide along walls
			glm::vec3 try_x = camera->transform->position;
			try_x.x += move.x;
			try_x.z = camera_height;
			if (!hits_wall(glm::vec2(try_x))) {
				camera->transform->position.x = try_x.x;
			}
			glm::vec3 try_y = camera->transform->position;
			try_y.y += move.y;
			try_y.z = camera_height;
			if (!hits_wall(glm::vec2(try_y))) {
				camera->transform->position.y = try_y.y;
			}
		}
	}

	// catch the player: in range, ghost looking at player, no wall between
	glm::vec2 player(camera->transform->position);
	constexpr float CatchRange = 5.0f;
	constexpr float FaceDot = 0.85f; // ghost facing player
	auto caught_by = [&](Scene::Transform *g) {
		if (!g) return false;
		glm::mat4x3 gw = g->make_world_from_local();
		glm::vec2 gp(gw[3]);
		glm::vec2 to_player = player - gp;
		float dist = glm::length(to_player);
		if (dist >= CatchRange || dist < 1e-4f) return false;
		to_player /= dist;

		glm::vec2 ghost_fwd(gw[0]);
		float glen = glm::length(ghost_fwd);
		if (glen < 1e-4f) return false;
		ghost_fwd /= glen;
		if (glm::dot(ghost_fwd, to_player) < FaceDot) return false;

		// clear line of sight
		if (wall_blocks(player, gp)) return false;

		return true;
	};
	bool caught = false;
	for (auto &g : ghosts) {
		if (caught_by(g.transform)) {
			caught = true;
			break;
		}
	}
	if (caught) {
		lost = true;
		for (auto &g : ghosts) {
			if (g.loop) g.loop->stop();
		}
		if (bgm_loop) bgm_loop->stop();
		if (drum_oneshot) drum_oneshot->stop();
	}

	// pick up key
	if (key && !have_key) {
		glm::vec2 key_pos(key->make_world_from_local()[3]);
		if (glm::length(player - key_pos) < PickupRadius) {
			have_key = true;
			key->scale = glm::vec3(0.0f); // hide
		}
	}

	// reach exit: win only with key
	if (exit_transform) {
		glm::vec2 exit_pos(exit_transform->make_world_from_local()[3]);
		if (glm::length(player - exit_pos) < PickupRadius) {
			if (have_key) {
				won = true;
				for (auto &g : ghosts) {
					if (g.loop) g.loop->stop();
				}
				if (bgm_loop) bgm_loop->stop();
			} else {
				need_key_message = true;
			}
		}
	}

	//update listener to camera position:
	glm::mat4x3 listen_frame = camera->transform->make_parent_from_local();
	glm::vec3 frame_right = listen_frame[0];
	glm::vec3 frame_at = listen_frame[3];
	Sound::listener.set_position_right(frame_at, frame_right, 1.0f / 60.0f);

	left.downs = 0;
	right.downs = 0;
	up.downs = 0;
	down.downs = 0;
}

void PlayMode::draw(glm::uvec2 const &drawable_size) {
	//update camera aspect ratio for drawable:
	camera->aspect = float(drawable_size.x) / float(drawable_size.y);

	//flashlight attached to the camera
	glm::mat4x3 cam = camera->transform->make_world_from_local();
	glm::vec3 light_location = glm::vec3(cam[3]);
	glm::vec3 light_direction = -glm::normalize(glm::vec3(cam[2]));
	glm::vec3 light_energy = glm::vec3(5.0f, 4.0f, 3.0f);
	float light_cutoff = std::cos(glm::radians(15.0f));

	glUseProgram(lit_color_texture_program->program);
	glUniform1i(lit_color_texture_program->LIGHT_TYPE_int, 2); // spot
	glUniform3fv(lit_color_texture_program->LIGHT_LOCATION_vec3, 1, glm::value_ptr(light_location));
	glUniform3fv(lit_color_texture_program->LIGHT_DIRECTION_vec3, 1, glm::value_ptr(light_direction));
	glUniform3fv(lit_color_texture_program->LIGHT_ENERGY_vec3, 1, glm::value_ptr(light_energy));
	glUniform1f(lit_color_texture_program->LIGHT_CUTOFF_float, light_cutoff);
	glUseProgram(0);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

	glClearDepth(1.0f); //1.0 is actually the default value to clear the depth buffer to, but FYI you can change it.
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS); //this is the default depth comparison function, but FYI you can change it.

	scene.draw(*camera);

	{ //use DrawLines to overlay some text:
		glDisable(GL_DEPTH_TEST);
		float aspect = float(drawable_size.x) / float(drawable_size.y);
		DrawLines lines(glm::mat4(
			1.0f / aspect, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f
		));

		constexpr float H = 0.09f;
		char const *msg = nullptr;
		if (showing_intro) {
			constexpr float IH = 0.07f;
			float line = 0.45f;
			auto draw_line = [&](char const *text) {
				lines.draw_text(text,
					glm::vec3(-aspect + 0.15f, line, 0.0),
					glm::vec3(IH, 0.0f, 0.0f), glm::vec3(0.0f, IH, 0.0f),
					glm::u8vec4(0xff, 0xff, 0xff, 0x00));
				line -= IH * 1.35f;
			};
			draw_line("You wake in a lightless maze.");
			draw_line("Ghosts haunt these halls.");
			draw_line("If they see you, you die.");
			draw_line("Find the key. Reach the exit.");
			draw_line("Luckily, you have a gun...");
			draw_line("but it seems unable to kill ghosts.");
			line -= IH * 0.6f;
			draw_line("Press Space to begin.");
		} else if (won) {
			msg = "You win! Press R to restart.";
		} else if (lost) {
			msg = "You died! Press R to restart.";
		} else if (need_key_message) {
			msg = "The exit is locked. Find the key!";
		} else if (shoot_message_timer > 0.0f) {
			msg = "You shoot!";
		} else if (have_key) {
			msg = "Key acquired! Find the exit.";
		} else {
			msg = "WASD to move, Space to shoot.";
		}
		if (msg) {
			float ofs = 2.0f / drawable_size.y;
			lines.draw_text(msg,
				glm::vec3(-aspect + 0.1f * H + ofs, -0.9 + 0.1f * H + ofs, 0.0),
				glm::vec3(H, 0.0f, 0.0f), glm::vec3(0.0f, H, 0.0f),
				glm::u8vec4(0xff, 0xff, 0xff, 0x00));
		}
	}
}
