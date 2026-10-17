/*
 * Seedee5 - a second controller for the CD-i pointer, added to the pad's own (cdiwheel.cpp's passPad):
 * the next signed-in user's controller moves the same pointer and presses the same buttons (the CD-i core has one
 * pointing device: the two take turns, or help).
 *
 * The choice is kept in saves/controls.txt; Settings > Controls changes it. Included by the program.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

struct ControlPrefs
{
	bool second{ true };
};

class Controls
{
public:
	ControlPrefs prefs;

	void load(const std::string &path)
	{
		file = path;
		FILE *f = std::fopen(path.c_str(), "r");
		if (!f) {
			return;
		}
		char line[128];
		while (std::fgets(line, sizeof line, f)) {
			int v;
			if (std::sscanf(line, "second_player=%d", &v) == 1) {
				prefs.second = v != 0;
			}
		}
		std::fclose(f);
	}

	void save() const
	{
		FILE *f = std::fopen(file.c_str(), "w");
		if (!f) {
			return;
		}
		std::fprintf(f, "second_player=%d\n", prefs.second ? 1 : 0);
		std::fclose(f);
		chmod(file.c_str(), 0666);
	}

	// Adds this frame's second controller to `out` (the pad as passPad made it)
	void apply(cdie_pad &out, uint32_t (*mapButtons)(uint32_t))
	{
		if (!prefs.second) {
			return;
		}
		struct pad p1;
		if (!pad_player(1, &p1)) {
			return;
		}
		out.buttons |= mapButtons(p1.held);
		auto add = [](int16_t a, float b) {
			return static_cast<int16_t>(std::clamp(static_cast<int>(a) + static_cast<int>(b * 32767.0f), -32767, 32767));
		};
		out.lx = add(out.lx, p1.left_x);
		out.ly = add(out.ly, p1.left_y);
		out.rx = add(out.rx, p1.right_x);
		out.ry = add(out.ry, p1.right_y);
		out.l2 = std::max(out.l2, static_cast<uint8_t>(std::clamp(p1.l2, 0.0f, 1.0f) * 255.0f));
		out.r2 = std::max(out.r2, static_cast<uint8_t>(std::clamp(p1.r2, 0.0f, 1.0f) * 255.0f));
	}

private:
	std::string file;
};
