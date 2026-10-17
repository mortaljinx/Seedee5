/*
 * Seedee5 - the settings screen: everything someone who has just installed the title needs, from the console.
 *
 *   Settings     the BIOS, the NAS, the games on the console, cover downloads, the DNS server for them, rescan
 *   NAS          on / off, find NFS servers on the network or type an address (digit by digit, no keyboard),
 *                pick a share and a folder (each folder says how many games it holds), NFS version, reserved
 *                port, room for copied games
 *   BIOS         where cdimono1.zip goes, and "Find it on my NAS" (searches the share and copies it in)
 *   Games        where games on the console go, and how to name multi-disc games
 *
 * The network work (finding servers, listing shares and folders, searching and copying) runs on the engine's
 * settings thread (cdie_net_*); this screen polls it every frame and never waits. Included by cdiwheel.cpp, so it
 * is compiled inside the program's namespace with everything prelude.h brings.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

class SettingsScreen
{
public:
	bool open{ false };
	Controls *controls{ nullptr }; // the program's (a second controller)
	bool rescanAsked{ false }; // the program asks the engine to look for games again

	void show(int gamesHere)
	{
		open = true;
		games = gamesHere;
		stack.clear();
		push(Page::main);
	}

	void close()
	{
		cdie_net_cancel();
		open = false;
	}

	// One frame of input. The caller does not pass the pad to anything else while the screen is open.
	void update(const hui::InputFrame &input, hui::ui::Feedback &feedback, int gamesHere)
	{
		games = gamesHere;
		pollJob();
		Page &p = stack.back();
		if (p.kind == Page::ip) {
			updateIp(input, feedback);
			return;
		}
		rebuild();
		const int n = static_cast<int>(rows.size());
		if (input.is_pressed(hui::Action::back) || input.is_pressed(hui::Action::menu)) {
			feedback.play(hui::audio::Cue::back);
			if (input.is_pressed(hui::Action::menu) || stack.size() == 1) {
				close();
			} else {
				pop();
			}
			return;
		}
		if (n == 0) {
			return;
		}
		if (input.nav == hui::Direction::up) {
			p.sel = (p.sel + n - 1) % n;
			feedback.play(hui::audio::Cue::focus);
		} else if (input.nav == hui::Direction::down) {
			p.sel = (p.sel + 1) % n;
			feedback.play(hui::audio::Cue::focus);
		}
		p.sel = std::clamp(p.sel, 0, n - 1);
		const Row row = rows[static_cast<size_t>(p.sel)];
		const int d = input.nav == hui::Direction::right ? 1 : input.nav == hui::Direction::left ? -1 : 0;
		if (d && row.change) {
			change(row.id, d);
			feedback.play(hui::audio::Cue::slider);
		}
		if (input.is_pressed(hui::Action::confirm) && row.enabled) {
			feedback.play(hui::audio::Cue::select);
			act(row);
		}
	}

	void draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts)
	{
		const hui::gfx::Color white{ 1, 1, 1, 1 };
		const hui::gfx::Color dim{ 1, 1, 1, 0.55f };
		const hui::gfx::Color accent{ 0.55f, 0.75f, 1.0f, 1.0f };
		list.rounded_rect({ -8.0f, -8.0f, 1936.0f, 1096.0f }, 4.0f, hui::gfx::Color{ 0.0f, 0.0f, 0.02f, 0.9f }); // the wheel and its hints dim underneath
		const hui::gfx::Rect r{ 300.0f, 70.0f, 1320.0f, 900.0f };
		list.shadow({ r.x, r.y + 14.0f, r.w, r.h }, 30.0f, 40.0f, hui::gfx::Color{ 0, 0, 0, 0.5f });
		list.rounded_rect(r, 30.0f, hui::gfx::Color{ 0.035f, 0.04f, 0.075f, 0.97f });
		list.bordered_rect(r, 30.0f, hui::gfx::Color{ 0, 0, 0, 0 }, 1.5f, hui::gfx::Color{ 1, 1, 1, 0.2f });
		const Page &p = stack.back();
		hui::ui::text(list, fonts.semibold, crumb().c_str(), r.x + 48.0f, r.y + 62.0f, 16, dim, hui::gfx::Align::left, 3.0f);
		hui::ui::text(list, fonts.display, title(p.kind), r.x + 48.0f, r.y + 116.0f, 44, white);
		float y = r.y + 150.0f;
		// a few lines of explanation, where a page has them
		for (const std::string &line : notes()) {
			// paragraph() returns the next line's baseline: the text ends a line above it
			y = hui::ui::paragraph(list, fonts.regular, line, r.x + 48.0f, y + 40.0f, 24, r.w - 96.0f, 34, white.with_alpha(0.75f), 3) - 34.0f;
		}
		if (p.kind == Page::ip) {
			drawIp(list, fonts, r, y + 40.0f);
		} else {
			y += 30.0f;
			const float rowH = 54.0f;
			const int visible = std::max(3, static_cast<int>((r.y + r.h - 130.0f - y) / rowH));
			const int n = static_cast<int>(rows.size());
			int first = std::clamp(p.sel - visible / 2, 0, std::max(0, n - visible));
			for (int i = first; i < n && i < first + visible; i++) {
				const Row &row = rows[static_cast<size_t>(i)];
				const bool on = i == p.sel;
				const hui::gfx::Rect box{ r.x + 28.0f, y, r.w - 56.0f, rowH - 6.0f };
				if (on) {
					list.rounded_rect(box, 16.0f, row.enabled ? white : white.with_alpha(0.25f));
				}
				const hui::gfx::Color ink = on ? hui::gfx::Color{ 0.04f, 0.05f, 0.09f, 1.0f } : white.with_alpha(row.enabled ? 1.0f : 0.4f);
				hui::ui::text(list, fonts.semibold, fonts.semibold.font->fit(row.label, 24, 640.0f), box.x + 22.0f, box.y + 33.0f, 24, ink);
				if (!row.value.empty()) {
					const std::string v = on && row.change ? "<  " + row.value + "  >" : row.value;
					hui::ui::text(list, fonts.regular, fonts.regular.font->fit(v, 22, 560.0f), box.x + box.w - 22.0f, box.y + 32.0f, 22,
						ink.with_alpha(on ? 1.0f : 0.62f), hui::gfx::Align::right);
				}
				y += rowH;
			}
			if (n > visible) {
				char more[48];
				std::snprintf(more, sizeof more, "%d of %d", p.sel + 1, n);
				hui::ui::text(list, fonts.mono, more, r.x + r.w - 48.0f, r.y + 116.0f, 20, dim, hui::gfx::Align::right);
			}
		}
		// what the network job is doing, or the last thing that happened
		const std::string status = jobState == 0 || backupBusy ? jobMsg : message;
		if (!status.empty()) {
			const hui::gfx::Color c = jobState == 0 || backupBusy ? accent : messageBad ? hui::gfx::Color{ 0.95f, 0.65f, 0.35f, 1.0f } : hui::gfx::Color{ 0.5f, 0.85f, 0.6f, 1.0f };
			hui::ui::text(list, fonts.regular, fonts.regular.font->fit(status, 22, r.w - 96.0f), r.x + 48.0f, r.y + r.h - 40.0f, 22, c);
		}
		// the wheel's own hints are under this row: a plate hides them
		list.rounded_rect({ -8.0f, 975.0f, 1936.0f, 120.0f }, 4.0f, hui::gfx::Color{ 0.0f, 0.0f, 0.02f, 1.0f });
		if (p.kind == Page::ip) {
			const hui::ui::Hint hints[] = { { hui::ui::Button::dpad, "Digit" }, { hui::ui::Button::l1, "Step 10", hui::ui::Button::r1 },
				{ hui::ui::Button::cross, "OK" }, { hui::ui::Button::circle, "Back" }, { hui::ui::Button::square, ipAutoLabel() } };
			hui::ui::draw_hints(list, fonts, hui::ui::GlyphStyle::dark(), hints, ipAllowsAuto() ? 5 : 4, 1824, true);
		} else {
			const hui::ui::Hint hints[] = { { hui::ui::Button::cross, "Choose" }, { hui::ui::Button::dpad, "Change" },
				{ hui::ui::Button::circle, stack.size() > 1 ? "Back" : "Close" } };
			hui::ui::draw_hints(list, fonts, hui::ui::GlyphStyle::dark(), hints, 3, 1824, true);
		}
	}

private:
	struct Page {
		enum Kind { main, nas, servers, shares, folders, ip, bios, biosfind, games, controls } kind;
		int sel{ 0 };
		std::string path; // folders: the folder shown
	};
	enum Id { idBios, idNas, idGames, idCovers, idCoversRetry, idDns, idRescan, idClose, idNasOn, idFind, idEnter, idServer, idShare,
		idFolder, idVersion, idReserved, idCache, idSave, idItem, idUseFolder, idUp, idBiosFind, idBiosCheck, idEnterInstead, idControls, idFace0, idFace1, idFace2, idFace3, idSecond, idBackup, idBackupNow, idRestore, idAbout };
	struct Row {
		Id id;
		std::string label, value;
		bool change{ false };  // left / right change it
		bool enabled{ true };
		int item{ -1 };        // which network item
	};

	std::vector<Page> stack;
	std::vector<Row> rows;
	bool backupBusy{ false };
	int games{ 0 };
	cdie_nas_cfg draft{};
	bool draftLoaded{ false };
	std::string message;
	bool messageBad{ false };
	int jobState{ -1 };
	int jobWhat{ -1 };
	std::string jobMsg;
	uint64_t jobDone{ 0 }, jobTotal{ 0 };
	std::vector<cdie_net_item> items;
	// the address being entered
	int ipDigits[4]{ 192, 168, 1, 1 };
	int ipAt{ 0 };
	bool ipForDns{ false };

	void push(Page::Kind k, const std::string &path = std::string())
	{
		Page p;
		p.kind = k;
		p.path = path;
		stack.push_back(p);
		message.clear();
		if (k == Page::nas || !draftLoaded) {
			cdie_nas_get(&draft);
			draftLoaded = true;
			if (draft.cache_gb < 1) {
				draft.cache_gb = 10;
			}
		}
	}

	void pop()
	{
		cdie_net_cancel();
		if (stack.size() > 1) {
			stack.pop_back();
		}
		message.clear();
	}

	void say(const std::string &m, bool bad = false)
	{
		message = m;
		messageBad = bad;
	}

	void startJob(int what, const char *ip, const char *share, const char *path)
	{
		items.clear();
		if (cdie_net_start(what, ip, share, path) == 0) {
			jobState = 0;
			jobWhat = what;
			jobMsg = "Working ...";
		} else {
			say("Busy: try again in a moment", true);
		}
	}

	void pollJob()
	{
		if (backupBusy) { // saves to or from the NAS
			char m[300];
			const int st = cdie_backup_poll(m, sizeof m);
			jobMsg = m;
			if (st != 0) {
				backupBusy = false;
				say(m, st != 1);
			}
		}
		if (jobState != 0) {
			return;
		}
		char msg[300];
		const int st = cdie_net_poll(msg, sizeof msg, &jobDone, &jobTotal);
		jobMsg = msg;
		if (st == 0) {
			return;
		}
		jobState = st;
		static cdie_net_item buf[512];
		const int n = cdie_net_items(buf, 512);
		items.assign(buf, buf + n);
		say(msg, st != 1);
		if (jobWhat == CDIE_NET_BIOS_COPY && st == 1) {
			pop(); // back to the BIOS page, which now says it is there
			say(msg);
		}
	}

	std::string crumb() const
	{
		std::string c = "SEEDEE5";
		for (size_t i = 1; i < stack.size(); i++) {
			c += "  /  ";
			c += hui::ui::upper(title(stack[i - 1].kind));
		}
		return c;
	}

	static const char *title(Page::Kind k)
	{
		switch (k) {
		case Page::main: return "Settings";
		case Page::nas: return "NAS";
		case Page::servers: return "NFS servers on your network";
		case Page::shares: return "Shares";
		case Page::folders: return "Pick the folder with your games";
		case Page::ip: return "Address";
		case Page::bios: return "CD-i BIOS";
		case Page::biosfind: return "Find the BIOS on the NAS";
		case Page::games: return "Games on this console";
		case Page::controls: return "Controls";
		}
		return "";
	}

	std::vector<std::string> notes() const
	{
		const Page &p = stack.back();
		cdie_bios b;
		cdie_bios_status(&b);
		std::vector<std::string> out;
		switch (p.kind) {
		case Page::controls:
			out.push_back("The CD-i has two buttons and a pointer. In a game: Cross, Circle, Square and Triangle as set here; the D-pad and "
			              "both sticks move the pointer; L2 held rewinds (when Rewind is on in the menu); the touch pad's click opens the menu. A second signed-in "
			              "controller can move the pointer too.");
			break;
		case Page::bios:
			out.push_back(b.bios ? std::string("The BIOS is in place.") + (b.dvc ? " vmpega.rom is there too: digital video (FMV) games work."
			                                                                     : " vmpega.rom is not: games with digital video (FMV) will not start.")
			                     : "The BIOS is missing: no game starts without it. It is not included: it comes from your own CD-i player.");
			out.push_back(std::string("It is cdimono1.zip, the SAME_CDI / MAME set (cdi220b.rom and its servo and slave ROMs, and vmpega.rom for FMV). "
			                          "Copy it over FTP to ") + ftpPath(b.folder) + "/cdimono1.zip, or let the console find it on your NAS.");
			break;
		case Page::games:
			out.push_back(std::string("Copy games over FTP into ") + ftpPath(b.roms) + ": .chd, .cue with its .bin, .iso, or .7z. Folders inside it are fine.");
			out.push_back("A game on several discs: name the files \"Game (Disc 1)\", \"Game (Disc 2)\"... in one folder; the wheel shows it once and the touch pad menu swaps discs.");
			out.push_back("Box art: Game (Europe).png or .jpg, next to the game or in the covers folder. Missing ones are downloaded.");
			out.push_back(std::string("Everything Seedee5 keeps (games, BIOS, covers, saves, logs) is in ") + ftpPath(b.data) + ".");
			break;
		case Page::nas:
			out.push_back("Games on a NAS (NFS) are listed with the rest and copied to the console when you start them. The NAS must share the folder over NFS to this console's address.");
			break;
		case Page::servers:
			out.push_back("Every address on this console's network is asked whether it serves NFS. Pick your NAS.");
			break;
		case Page::folders:
			out.push_back(std::string(draft.server) + ":" + draft.share + (p.path.empty() ? "" : "/" + p.path));
			break;
		case Page::ip:
			out.push_back(ipForDns ? "The DNS server to ask for the cover servers' addresses (your router does it when this is automatic)."
			                       : "The NAS's address on your network (it is in the NAS's network settings, or your router's list of devices).");
			break;
		case Page::biosfind:
			out.push_back(std::string("Looking in ") + draft.server + ":" + draft.share + " for cdimono1.zip (six folders deep). A big share takes a while.");
			break;
		default:
			break;
		}
		return out;
	}

	void rebuild()
	{
		rows.clear();
		const Page &p = stack.back();
		cdie_bios b;
		cdie_bios_status(&b);
		char text[300];
		switch (p.kind) {
		case Page::controls: {
			int map[4];
			cdie_buttons_get(map);
			static const char *const face[4] = { "Cross", "Circle", "Square", "Triangle" };
			static const char *const what[4] = { "nothing", "button 1", "button 2", "both buttons" };
			for (int i = 0; i < 4; i++) {
				rows.push_back({ static_cast<Id>(idFace0 + i), face[i], what[map[i] & 3], true });
			}
			if (controls) {
				rows.push_back({ idSecond, "Second controller moves the pointer too", controls->prefs.second ? "on" : "off", true });
			}
			break;
		}
		case Page::main: {
			rows.push_back({ idBios, "CD-i BIOS", b.bios ? (b.dvc ? "found, with FMV" : "found (no FMV ROM)") : "MISSING" });
			cdie_nas_cfg c;
			cdie_nas_get(&c);
			std::string nas = !b.nfs ? "not in this build" : c.enabled ? std::string(c.server) + "  \xC2\xB7  " + c.share : "off";
			rows.push_back({ idNas, "NAS (games over the network)", nas });
			std::snprintf(text, sizeof text, "%d", games);
			rows.push_back({ idGames, "Games found (where to put them)", text });
			rows.push_back({ idCovers, "Download covers and game info", cdie_covers_on() ? "on" : "off", true });
			rows.push_back({ idCoversRetry, "Try the cover downloads again", "", false, cdie_covers_on() != 0 });
			char dns[64];
			cdie_dns_get(dns, sizeof dns);
			rows.push_back({ idDns, "DNS server for covers", dns[0] ? dns : "automatic" });
			rows.push_back({ idControls, "Controls", "buttons, second controller" });
			rows.push_back({ idRescan, "Look for games again", "" });
			rows.push_back({ idAbout, "Version", "Seedee5 " SEEDEE5_VERSION });
			rows.push_back({ idClose, "Close", "" });
			break;
		}
		case Page::nas: {
			rows.push_back({ idFind, "Find NAS on my network", "" });
			rows.push_back({ idEnter, "Enter the address", "" });
			const bool have = draft.server[0] && draft.share[0];
			rows.push_back({ idNasOn, "Use the NAS", draft.enabled ? "on" : "off", true, have });
			rows.push_back({ idServer, "Address", draft.server[0] ? draft.server : "-", false, false });
			rows.push_back({ idShare, "Share", draft.share[0] ? draft.share : "-", false, draft.server[0] != 0 });
			rows.push_back({ idFolder, "Folder with the games", draft.path[0] ? draft.path : "(the whole share)", false, have });
			static const char *const versions[] = { "automatic (3, then 4)", "NFS 3", "NFS 4" };
			rows.push_back({ idVersion, "NFS version", versions[draft.version == 3 ? 1 : draft.version == 4 ? 2 : 0], true });
			rows.push_back({ idReserved, "Connect from a reserved port", draft.reserved ? "on" : "off (share needs \"insecure\")", true });
			std::snprintf(text, sizeof text, "%d GB", draft.cache_gb);
			rows.push_back({ idCache, "Room for copied games", text, true });
			rows.push_back({ idBackup, "Back up saves to the NAS after each game", draft.backup ? "on" : "off", true, have });
			rows.push_back({ idBackupNow, "Back up saves now", "", false, have && !backupBusy });
			rows.push_back({ idRestore, "Bring saves back from the NAS", "", false, have && !backupBusy });
			rows.push_back({ idSave, "Save, and read the NAS", "", false, have || !draft.enabled });
			break;
		}
		case Page::servers:
			for (size_t i = 0; i < items.size(); i++) {
				const cdie_net_item &it = items[i];
				if (it.kind != CDIE_NET_SERVER) {
					continue;
				}
				std::snprintf(text, sizeof text, it.count < 0 ? "shares not listed" : "%d share%s", it.count, it.count == 1 ? "" : "s");
				rows.push_back({ idItem, it.name, text, false, true, static_cast<int>(i) });
			}
			rows.push_back({ idEnterInstead, "Enter the address instead", "" });
			break;
		case Page::shares:
			for (size_t i = 0; i < items.size(); i++) {
				if (items[i].kind == CDIE_NET_SHARE) {
					rows.push_back({ idItem, items[i].name, "", false, true, static_cast<int>(i) });
				}
			}
			break;
		case Page::folders: {
			const bool listed = jobState == 1 && jobWhat == CDIE_NET_FOLDER;
			text[0] = 0;
			if (listed) {
				std::snprintf(text, sizeof text, "%llu game file%s here", static_cast<unsigned long long>(jobDone), jobDone == 1 ? "" : "s");
			}
			rows.push_back({ idUseFolder, "Use this folder", text, false, listed });
			if (!p.path.empty()) {
				rows.push_back({ idUp, "..  (the folder above)", "" });
			}
			for (size_t i = 0; i < items.size(); i++) {
				if (items[i].kind == CDIE_NET_FOLDER_ITEM) {
					rows.push_back({ idItem, std::string(items[i].name) + "/", "", false, true, static_cast<int>(i) });
				}
			}
			break;
		}
		case Page::bios: {
			const bool nas = draft.server[0] && draft.share[0] && b.nfs;
			rows.push_back({ idBiosFind, "Find the BIOS on my NAS", nas ? "" : "set up the NAS first", false, nas });
			rows.push_back({ idBiosCheck, "Check again", "" });
			break;
		}
		case Page::biosfind:
			for (size_t i = 0; i < items.size(); i++) {
				if (items[i].kind == CDIE_NET_FILE) {
					std::snprintf(text, sizeof text, "%d KB  \xC2\xB7  copy it", items[i].count);
					rows.push_back({ idItem, items[i].path, text, false, true, static_cast<int>(i) });
				}
			}
			break;
		case Page::games:
			rows.push_back({ idRescan, "Look for games again", "" });
			break;
		case Page::ip:
			break;
		}
	}

	void change(Id id, int d)
	{
		switch (id) {
		case idCovers:
			cdie_covers_set(!cdie_covers_on());
			say(cdie_covers_on() ? "Covers and game info will be downloaded" : "No downloads: only what is already on the console");
			break;
		case idNasOn:
			draft.enabled = !draft.enabled;
			break;
		case idVersion: {
			static const int order[] = { 0, 3, 4 };
			int at = draft.version == 3 ? 1 : draft.version == 4 ? 2 : 0;
			draft.version = order[(at + d + 3) % 3];
			break;
		}
		case idReserved:
			draft.reserved = !draft.reserved;
			break;
		case idCache:
			draft.cache_gb = std::clamp(draft.cache_gb + 2 * d, 2, 512);
			break;
		case idBackup:
			draft.backup = !draft.backup;
			break;
		case idFace0:
		case idFace1:
		case idFace2:
		case idFace3: {
			int map[4];
			cdie_buttons_get(map);
			const int i = id - idFace0;
			map[i] = (map[i] + d + 4) % 4;
			if (cdie_buttons_set(map) == 0) {
				say("Used from the next game started");
			} else {
				say(std::string("Not saved: ") + cdie_last_error(), true);
			}
			break;
		}
		default:
			if (controls) {
				ControlPrefs &c = controls->prefs;
				switch (id) {
				case idSecond: c.second = !c.second; break;
				default: return;
				}
				controls->save();
			}
			break;
		}
	}

	void saveNas(const char *done)
	{
		if (cdie_nas_set(&draft) == 0) {
			say(done); // the engine reads the NAS again by itself
		} else {
			say(std::string("Not saved: ") + cdie_last_error(), true);
		}
	}

	void act(const Row &row)
	{
		cdie_bios b;
		cdie_bios_status(&b);
		switch (row.id) {
		case idBios:
			push(Page::bios);
			break;
		case idControls:
			push(Page::controls);
			break;
		case idFace0: case idFace1: case idFace2: case idFace3: case idSecond:
			change(row.id, 1);
			break;
		case idNas:
			if (!b.nfs) {
				say("This build was made without NAS support", true);
				break;
			}
			push(Page::nas);
			break;
		case idGames:
			push(Page::games);
			break;
		case idCovers:
			change(idCovers, 1);
			break;
		case idCoversRetry:
			cdie_covers_retry();
			say("Trying the downloads again (the wheel's top line shows how they go)");
			break;
		case idDns: {
			char dns[64];
			cdie_dns_get(dns, sizeof dns);
			openIp(true, dns[0] ? dns : "192.168.1.1");
			break;
		}
		case idRescan:
			rescanAsked = true;
			say("Looking for games again");
			break;
		case idClose:
			close();
			break;
		case idFind:
			push(Page::servers);
			startJob(CDIE_NET_FIND, nullptr, nullptr, nullptr);
			break;
		case idEnter:
		case idEnterInstead:
			openIp(false, draft.server[0] ? draft.server : "192.168.1.1");
			break;
		case idShare:
			push(Page::shares);
			startJob(CDIE_NET_SHARES, draft.server, nullptr, nullptr);
			break;
		case idFolder:
			openFolder(draft.path);
			break;
		case idNasOn:
		case idVersion:
		case idReserved:
		case idCache:
		case idBackup:
			change(row.id, 1);
			break;
		case idBackupNow:
		case idRestore:
			// the share as saved (Save first after changing it)
			if (cdie_backup_start(row.id == idRestore ? 1 : 0) == 0) {
				backupBusy = true;
				say(row.id == idRestore ? "Bringing saves back from the NAS ..." : "Copying saves to the NAS ...");
			} else {
				say("Not now: a copy is running, or the NAS is not set up (Save first)", true);
			}
			break;
		case idSave:
			saveNas(draft.enabled ? "Saved: reading the NAS (the wheel's top line shows how it goes)" : "Saved: the NAS is off");
			break;
		case idUseFolder:
			std::snprintf(draft.path, sizeof draft.path, "%s", stack.back().path.c_str());
			draft.enabled = 1;
			while (stack.size() > 1 && stack.back().kind != Page::nas) {
				stack.pop_back();
			}
			saveNas("Saved: reading the games on the NAS (the wheel's top line shows how it goes)");
			break;
		case idUp: {
			std::string up = stack.back().path;
			const size_t slash = up.rfind('/');
			up = slash == std::string::npos ? std::string() : up.substr(0, slash);
			stack.back().path = up;
			stack.back().sel = 0;
			startJob(CDIE_NET_FOLDER, draft.server, draft.share, up.c_str());
			break;
		}
		case idItem: {
			if (row.item < 0 || row.item >= static_cast<int>(items.size())) {
				break;
			}
			const cdie_net_item it = items[static_cast<size_t>(row.item)];
			if (it.kind == CDIE_NET_SERVER) {
				std::snprintf(draft.server, sizeof draft.server, "%s", it.path);
				draft.share[0] = 0;
				draft.path[0] = 0;
				push(Page::shares);
				stack.back().path.clear();
				startJob(CDIE_NET_SHARES, draft.server, nullptr, nullptr);
			} else if (it.kind == CDIE_NET_SHARE) {
				std::snprintf(draft.share, sizeof draft.share, "%s", it.path);
				draft.path[0] = 0;
				openFolder("");
			} else if (it.kind == CDIE_NET_FOLDER_ITEM) {
				stack.back().path = it.path;
				stack.back().sel = 0;
				startJob(CDIE_NET_FOLDER, draft.server, draft.share, it.path);
			} else if (it.kind == CDIE_NET_FILE) {
				startJob(CDIE_NET_BIOS_COPY, draft.server, draft.share, it.path);
			}
			break;
		}
		case idBiosFind:
			push(Page::biosfind);
			startJob(CDIE_NET_BIOS_FIND, draft.server, draft.share, "");
			break;
		case idBiosCheck:
			say(b.bios ? "The BIOS is in place" : "Still missing", !b.bios);
			break;
		default:
			break;
		}
	}

	void openFolder(const std::string &path)
	{
		// the folder page replaces a shares page, so Back from it goes to the NAS page
		if (stack.back().kind == Page::shares) {
			stack.pop_back();
		}
		push(Page::folders, path);
		startJob(CDIE_NET_FOLDER, draft.server, draft.share, path.c_str());
	}

	// ---- the address entry: four numbers, no keyboard ----

	bool ipAllowsAuto() const
	{
		return ipForDns;
	}

	const char *ipAutoLabel() const
	{
		return "Automatic";
	}

	void openIp(bool forDns, const char *start)
	{
		ipForDns = forDns;
		unsigned v[4] = { 192, 168, 1, 1 };
		std::sscanf(start, "%u.%u.%u.%u", &v[0], &v[1], &v[2], &v[3]);
		for (int i = 0; i < 4; i++) {
			ipDigits[i] = static_cast<int>(std::min(v[i], 255u));
		}
		ipAt = 3;
		push(Page::ip);
	}

	void updateIp(const hui::InputFrame &input, hui::ui::Feedback &feedback)
	{
		if (input.is_pressed(hui::Action::back)) {
			feedback.play(hui::audio::Cue::back);
			pop();
			return;
		}
		if (input.nav == hui::Direction::left) {
			ipAt = (ipAt + 3) % 4;
		} else if (input.nav == hui::Direction::right) {
			ipAt = (ipAt + 1) % 4;
		}
		int step = 0;
		if (input.nav == hui::Direction::up) step = 1;
		if (input.nav == hui::Direction::down) step = -1;
		if (input.is_pressed(hui::Action::page_next)) step = 10;
		if (input.is_pressed(hui::Action::page_prev)) step = -10;
		if (step) {
			ipDigits[ipAt] = (ipDigits[ipAt] + step + 256) % 256;
			feedback.play(hui::audio::Cue::tick);
		} else if (input.nav != hui::Direction::none) {
			feedback.play(hui::audio::Cue::focus);
		}
		char ip[32];
		std::snprintf(ip, sizeof ip, "%d.%d.%d.%d", ipDigits[0], ipDigits[1], ipDigits[2], ipDigits[3]);
		if (ipForDns && input.is_pressed(hui::Action::west)) {
			cdie_dns_set("");
			feedback.play(hui::audio::Cue::select);
			pop();
			say("Covers: names are looked up through your router again");
			return;
		}
		if (input.is_pressed(hui::Action::confirm)) {
			feedback.play(hui::audio::Cue::select);
			if (ipForDns) {
				cdie_dns_set(ip);
				pop();
				say(std::string("Covers: names are looked up at ") + ip);
				cdie_covers_retry();
			} else {
				std::snprintf(draft.server, sizeof draft.server, "%s", ip);
				draft.share[0] = 0;
				draft.path[0] = 0;
				pop();
				push(Page::shares);
				startJob(CDIE_NET_SHARES, draft.server, nullptr, nullptr);
			}
		}
	}

	void drawIp(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const hui::gfx::Rect &r, float y)
	{
		const float boxW = 170.0f, gap = 40.0f;
		float x = r.x + (r.w - (4.0f * boxW + 3.0f * gap)) * 0.5f;
		for (int i = 0; i < 4; i++) {
			const bool on = i == ipAt;
			const hui::gfx::Rect box{ x, y, boxW, 120.0f };
			list.rounded_rect(box, 22.0f, on ? hui::gfx::Color{ 1, 1, 1, 1 } : hui::gfx::Color{ 1, 1, 1, 0.08f });
			char t[8];
			std::snprintf(t, sizeof t, "%d", ipDigits[i]);
			hui::ui::text(list, fonts.mono, t, box.x + boxW * 0.5f, box.y + 82.0f, 60,
				on ? hui::gfx::Color{ 0.04f, 0.05f, 0.09f, 1.0f } : hui::gfx::Color{ 1, 1, 1, 0.9f }, hui::gfx::Align::center);
			if (on) {
				hui::ui::text(list, fonts.regular, "up / down", box.x + boxW * 0.5f, box.y + 160.0f, 20, hui::gfx::Color{ 1, 1, 1, 0.5f },
					hui::gfx::Align::center);
			}
			if (i < 3) {
				hui::ui::text(list, fonts.display, ".", x + boxW + gap * 0.5f, y + 96.0f, 60, hui::gfx::Color{ 1, 1, 1, 0.7f }, hui::gfx::Align::center);
			}
			x += boxW + gap;
		}
	}
};
