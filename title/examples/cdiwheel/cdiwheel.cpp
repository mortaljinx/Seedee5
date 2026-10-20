/*
 * Seedee5 - its program: the Philips CD-i emulator of ps5-cdi as a native PS5 title, drawn with Vulkan on
 * the PS5 Vulkan Template, its game list on the UI kit's Cover Wheel.
 *
 * Everything that is not drawing is the engine (engine/cdie.h, shared with ps5-cdi's SDL payload): the game
 * list from /data/ps5-cdi/roms and the NAS, the covers, copying and unpacking, and the SAME_CDI core on a
 * thread of its own. This program:
 *   - builds the wheel's list (cdi::library(), read by kit/screen.cpp) from the engine's, turns the cover
 *     pictures the engine reads into textures near the wheel's centre (and lets go of the far ones), and keeps
 *     the favourites (/data/ps5-cdi/saves/favourites.txt);
 *   - when Play is pressed, gets the game ready (a progress card while it is copied or unpacked) and starts it;
 *   - while a game runs, hands the pad to the engine and shows its frames: two Vulkan images (the newest frame
 *     and the one before it) drawn as the kit's textures, the newest over the previous at the engine's blend,
 *     so a 50 Hz game moves smoothly on a 60 Hz screen; the touch pad pauses it under a menu (save states,
 *     discs, pointer, picture and sound settings, back to the list);
 *   - plays the game's sound and the kit's sounds on the console's one output.
 * Holding OPTIONS for a second on the wheel closes the title.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "kit.hpp"
#include <type_traits>
static std::string ftpPath(const std::string &path); // (below: a path as FTP shows it)
#include "controls.hpp"
#include "logo_png.h"
#include "settings.hpp"

// The design on screen: this title's own, in kit/screen.cpp
std::unique_ptr<hui::app::Concept> makeScreen(hui::app::Context &context)
{
	return ps5ui::make_screen(context);
}

// ---- the log: klog (standard error) and <data folder>/saves/title_log.txt ----
static FILE *g_logFile;
static std::mutex g_noticeMutex;
static std::string g_notice;   // the newest short news, shown at the top of the screen for a few seconds
static double g_noticeAt = -1e9;

static void logLine(const char *line)
{
	std::fprintf(stderr, "%s\n", line);
	if (g_logFile) {
		std::fprintf(g_logFile, "%s\n", line);
		std::fflush(g_logFile);
	}
}

static void noticeLine(const char *line)
{
	std::lock_guard<std::mutex> hold(g_noticeMutex);
	g_notice = line;
	g_noticeAt = cdie_now_ms();
}

// ---- the game's picture ----
// One Vulkan image a frame of the emulator is copied into; the kit draws it like any texture (import_texture).
// It rests in SHADER_READ_ONLY_OPTIMAL, as the kit requires whenever a frame draws it. (Jellyfin Wheel's
// VideoSurface, sized for a CD-i frame: at most 768 x 576.)
struct VideoSurface
{
	static constexpr int kWidth = 1024;
	static constexpr int kHeight = 640;

	VkDevice dev{ VK_NULL_HANDLE };
	VkQueue queue{ VK_NULL_HANDLE };
	VkImage image{ VK_NULL_HANDLE };
	VkDeviceMemory imageMemory{ VK_NULL_HANDLE };
	VkImageView view{ VK_NULL_HANDLE };
	VkBuffer staging{ VK_NULL_HANDLE };
	VkDeviceMemory stagingMemory{ VK_NULL_HANDLE };
	void *mapped{ nullptr };
	VkCommandPool pool{ VK_NULL_HANDLE };
	VkCommandBuffer cmd{ VK_NULL_HANDLE };
	VkFence fence{ VK_NULL_HANDLE };

	static uint32_t findMemory(const VkPhysicalDeviceMemoryProperties &props, uint32_t bits, VkMemoryPropertyFlags want)
	{
		for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
			if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) {
				return i;
			}
		}
		return UINT32_MAX;
	}

	void beginCommands()
	{
		vkResetCommandBuffer(cmd, 0);
		VkCommandBufferBeginInfo begin{};
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cmd, &begin);
	}

	void barrier(VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
		VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
	{
		VkImageMemoryBarrier b{};
		b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		b.srcAccessMask = srcAccess;
		b.dstAccessMask = dstAccess;
		b.oldLayout = from;
		b.newLayout = to;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = image;
		b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
	}

	bool submitAndWait()
	{
		vkEndCommandBuffer(cmd);
		VkSubmitInfo submit{};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &cmd;
		if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS) {
			return false;
		}
		vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
		vkResetFences(dev, 1, &fence);
		return true;
	}

	bool create(VkPhysicalDevice physical, VkDevice device, VkQueue q, uint32_t family)
	{
		dev = device;
		queue = q;
		VkPhysicalDeviceMemoryProperties props;
		vkGetPhysicalDeviceMemoryProperties(physical, &props);

		VkImageCreateInfo ici{};
		ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = VK_FORMAT_R8G8B8A8_UNORM;
		ici.extent = { static_cast<uint32_t>(kWidth), static_cast<uint32_t>(kHeight), 1 };
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (vkCreateImage(dev, &ici, nullptr, &image) != VK_SUCCESS) {
			return false;
		}
		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(dev, image, &req);
		VkMemoryAllocateInfo alloc{};
		alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = findMemory(props, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &alloc, nullptr, &imageMemory) != VK_SUCCESS ||
			vkBindImageMemory(dev, image, imageMemory, 0) != VK_SUCCESS) {
			return false;
		}
		VkImageViewCreateInfo vci{};
		vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vci.image = image;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = ici.format;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		if (vkCreateImageView(dev, &vci, nullptr, &view) != VK_SUCCESS) {
			return false;
		}

		const VkDeviceSize bytes = static_cast<VkDeviceSize>(kWidth) * kHeight * 4;
		VkBufferCreateInfo bci{};
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.size = bytes;
		bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(dev, &bci, nullptr, &staging) != VK_SUCCESS) {
			return false;
		}
		vkGetBufferMemoryRequirements(dev, staging, &req);
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = findMemory(props, req.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &alloc, nullptr, &stagingMemory) != VK_SUCCESS ||
			vkBindBufferMemory(dev, staging, stagingMemory, 0) != VK_SUCCESS ||
			vkMapMemory(dev, stagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
			return false;
		}

		VkCommandPoolCreateInfo pci{};
		pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = family;
		VkCommandBufferAllocateInfo cai{};
		cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		VkFenceCreateInfo fci{};
		fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		if (vkCreateCommandPool(dev, &pci, nullptr, &pool) != VK_SUCCESS) {
			return false;
		}
		cai.commandPool = pool;
		if (vkAllocateCommandBuffers(dev, &cai, &cmd) != VK_SUCCESS || vkCreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS) {
			return false;
		}

		// black, and in the layout the kit expects
		beginCommands();
		barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkClearColorValue black{};
		VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
		barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		return submitAndWait();
	}

	// Copies a w x h RGBA picture (rows `pitch` bytes apart; w <= kWidth, h <= kHeight) into the image's top left
	// corner. Waits for the copy, so the pixels may change as soon as it returns.
	void upload(const uint8_t *rgba, int w, int h, int pitch)
	{
		if (!view || w <= 0 || h <= 0 || w > kWidth || h > kHeight) {
			return;
		}
		for (int y = 0; y < h; y++) {
			std::memcpy(static_cast<uint8_t *>(mapped) + static_cast<size_t>(y) * w * 4, rgba + static_cast<size_t>(y) * pitch,
				static_cast<size_t>(w) * 4);
		}
		beginCommands();
		barrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkBufferImageCopy region{};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.imageExtent = { static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1 };
		vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		submitAndWait();
	}

	// The caller has waited for the device to be idle, and has dropped the kit's handle for the view.
	void destroy()
	{
		if (!dev) {
			return;
		}
		if (fence) vkDestroyFence(dev, fence, nullptr);
		if (pool) vkDestroyCommandPool(dev, pool, nullptr);
		if (mapped) vkUnmapMemory(dev, stagingMemory);
		if (staging) vkDestroyBuffer(dev, staging, nullptr);
		if (stagingMemory) vkFreeMemory(dev, stagingMemory, nullptr);
		if (view) vkDestroyImageView(dev, view, nullptr);
		if (image) vkDestroyImage(dev, image, nullptr);
		if (imageMemory) vkFreeMemory(dev, imageMemory, nullptr);
		*this = VideoSurface();
	}
};

#include "picture.hpp" // the picture filters (Sharp, Smooth, Enhanced, Scanlines, CRT)

// The paths of everything the title keeps on the console (the host reference build: CDI_DATA_DIR, for tests)
#ifndef CDI_TITLE_ID
#define CDI_TITLE_ID "PPSA99142"
#endif

// Where the title keeps everything (games, BIOS, covers, saves, settings, logs). A title runs in a sandbox: /app0
// is its own folder, which FTP sees as /data/homebrew/<TITLE_ID>/, and the console's /data is not the /data FTP
// shows unless the title is elevated. So the home is /app0/data, which anyone can find over FTP inside the app's
// folder. The old payload's /data/ps5-cdi is the fallback when /app0 cannot be written.
static bool writableDir(const std::string &dir)
{
	mkdir(dir.c_str(), 0777);
	chmod(dir.c_str(), 0777);
	const std::string probe = dir + "/.write-test";
	FILE *f = std::fopen(probe.c_str(), "w");
	if (!f) {
		return false;
	}
	const bool ok = std::fputs("ok\n", f) >= 0;
	std::fclose(f);
	std::remove(probe.c_str());
	return ok;
}

static std::string dataPath(const char *sub)
{
	static std::string base;
	if (base.empty()) {
#if defined(PS5_HOST_REFERENCE)
		const char *d = getenv("CDI_DATA_DIR");
		base = d && *d ? d : "cdi-data";
		mkdir(base.c_str(), 0777);
#else
		base = writableDir("/app0/data") ? "/app0/data" : "/data/ps5-cdi";
#endif
	}
	return sub && *sub ? base + "/" + sub : base;
}

// A path as FTP shows it (/app0 is the app's folder under /data/homebrew), for what the screens tell people
static std::string ftpPath(const std::string &path)
{
#if defined(PS5_HOST_REFERENCE)
	return path;
#else
	if (path.rfind("/app0", 0) == 0) {
		return std::string("/data/homebrew/") + CDI_TITLE_ID + path.substr(5);
	}
	return path;
#endif
}

// First launch in the app's folder: bring over what the old payload's /data/ps5-cdi had (the BIOS and the NAS
// settings), when the title can read it. Copies only what is missing here.
static int copyFile(const std::string &from, const std::string &to)
{
	FILE *in = std::fopen(from.c_str(), "rb");
	if (!in) {
		return -1;
	}
	const std::string part = to + ".part";
	FILE *out = std::fopen(part.c_str(), "wb");
	if (!out) {
		std::fclose(in);
		return -1;
	}
	char buf[65536];
	size_t n;
	bool ok = true;
	while ((n = std::fread(buf, 1, sizeof buf, in)) > 0) {
		if (std::fwrite(buf, 1, n, out) != n) {
			ok = false;
			break;
		}
	}
	std::fclose(in);
	ok = std::fclose(out) == 0 && ok;
	if (!ok || std::rename(part.c_str(), to.c_str()) != 0) {
		std::remove(part.c_str());
		return -1;
	}
	chmod(to.c_str(), 0666);
	return 0;
}

static void importOldData(void (*log)(const char *))
{
#if !defined(PS5_HOST_REFERENCE)
	const std::string home = dataPath(nullptr);
	if (home == "/data/ps5-cdi") {
		return;
	}
	struct Item { const char *from, *to; };
	static const Item items[] = {
		{ "/data/ps5-cdi/system/same_cdi/bios/cdimono1.zip", "system/same_cdi/bios/cdimono1.zip" },
		{ "/data/ps5-cdi/nfs.cfg", "nfs.cfg" },
		{ "/data/ps5-cdi/saves/nfs.cfg", "nfs.cfg" },
		{ "/data/ps5-cdi/dns.txt", "dns.txt" },
		{ "/data/ps5-cdi/options.cfg", "options.cfg" },
		{ "/data/ps5-cdi/saves/view.cfg", "saves/view.cfg" },
		{ "/data/ps5-cdi/saves/favourites.txt", "saves/favourites.txt" },
	};
	for (const Item &it : items) {
		const std::string to = home + "/" + it.to;
		struct stat st;
		if (stat(to.c_str(), &st) == 0) {
			continue; // there already
		}
		for (std::string d = to.substr(0, to.rfind('/')), p; !d.empty();) { // its folders
			const size_t at = d.find('/', p.size() + 1);
			p = at == std::string::npos ? d : d.substr(0, at);
			mkdir(p.c_str(), 0777);
			chmod(p.c_str(), 0777);
			if (p == d) {
				break;
			}
		}
		if (copyFile(it.from, to) == 0) {
			char line[700];
			std::snprintf(line, sizeof line, "CDI: copied %s from the old payload's folder into %s", it.from, ftpPath(to).c_str());
			log(line);
		}
	}
#else
	(void)log;
#endif
}

// ---- what the details panel knows about a game: year, developer, publisher ----
#include "games_info.hpp"

class GameInfo
{
public:
	// the bundled list, then data/games_info.txt (the same "Title|Developer|Publisher|Year" lines; its entries win)
	void load(const std::string &userFile)
	{
		for (const char *line : kGamesInfo) {
			add(line);
		}
		if (FILE *f = std::fopen(userFile.c_str(), "r")) {
			char line[600];
			while (std::fgets(line, sizeof line, f)) {
				line[std::strcspn(line, "\r\n")] = 0;
				if (line[0] && line[0] != '#') {
					add(line);
				}
			}
			std::fclose(f);
		}
	}

	void fill(cdi::Game &game) const
	{
		const Entry *e = find(game.title);
		if (!e) {
			return;
		}
		game.year = e->year;
		game.developer = e->developer;
		game.publisher = e->publisher;
	}

	// The game's name as the list of CD-i games writes it ("The 7th Guest"), which is how its Wikipedia article is
	// most likely named; else the file's ("7th Guest, The" made "The 7th Guest")
	std::string articleTitle(const std::string &title) const
	{
		if (const Entry *e = find(title)) {
			if (!e->title.empty()) {
				return e->title;
			}
		}
		for (const char *article : { ", The", ", A", ", Le", ", La", ", L'" }) {
			const size_t n = std::strlen(article);
			if (title.size() > n && title.compare(title.size() - n, n, article) == 0) {
				const std::string word = article + 2;
				return word + (word.back() == '\'' ? "" : " ") + title.substr(0, title.size() - n);
			}
		}
		return title;
	}

private:
	struct Entry { std::string developer, publisher, year, title; };
	std::map<std::string, Entry> byKey;

	// "The 7th Guest" and "7th Guest, The" and "7TH GUEST" are one key: lower case letters and digits, no leading
	// article
	static std::string key(const std::string &title)
	{
		std::string t;
		for (const char c : title) {
			const unsigned char u = static_cast<unsigned char>(c);
			if (std::isalnum(u)) {
				t += static_cast<char>(std::tolower(u));
			} else if (u >= 0x80) {
				t += c; // accented letters as they are
			} else if (!t.empty() && t.back() != ' ') {
				t += ' ';
			}
		}
		while (!t.empty() && t.back() == ' ') {
			t.pop_back();
		}
		for (const char *article : { "the ", "a ", "l ", "la ", "le " }) {
			if (t.rfind(article, 0) == 0) {
				t = t.substr(std::strlen(article));
				break;
			}
		}
		for (const char *article : { " the", " a" }) { // "7th Guest, The"
			const size_t n = std::strlen(article);
			if (t.size() > n && t.compare(t.size() - n, n, article) == 0) {
				t.resize(t.size() - n);
				break;
			}
		}
		std::string out;
		for (const char c : t) {
			if (c != ' ') {
				out += c;
			}
		}
		return out;
	}

	void add(const std::string &line)
	{
		std::vector<std::string> f;
		size_t at = 0;
		for (;;) {
			const size_t bar = line.find('|', at);
			f.push_back(line.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
			if (bar == std::string::npos) {
				break;
			}
			at = bar + 1;
		}
		if (f.size() < 4 || f[0].empty()) {
			return;
		}
		byKey[key(f[0])] = Entry{ f[1], f[2], f[3], f[0] };
	}

	const Entry *find(const std::string &title) const
	{
		auto it = byKey.find(key(title));
		if (it != byKey.end()) {
			return &it->second;
		}
		// "Burn:Cycle" is "Burn Cycle"; "Mad Dog McCree - ..." / "Zelda - The Wand of Gamelon": the part before " - "
		const size_t dash = title.find(" - ");
		if (dash != std::string::npos) {
			std::string t = title;
			t[dash + 1] = ':';
			it = byKey.find(key(t));
			if (it != byKey.end()) {
				return &it->second;
			}
		}
		return nullptr;
	}
};

class VulkanExample : public ps5ui::KitExample
{
public:
	hui::Settings choices;
	hui::app::Telemetry telemetry;
	std::unique_ptr<hui::app::Context> context;
	std::unique_ptr<hui::app::Concept> screen;
	hui::app::Frame frame;
	hui::ui::Feedback feedback;
	ps5ui::HoldToLeave leaving;
	hui::gfx::DrawList leavingList, cardList, gameList, menuList, noticeList;

	enum class Mode { list, preparing, playing, failed };
	Mode mode{ Mode::list };
	bool engineOk{ false };
	std::string engineError;
	std::atomic<bool> gameSound{ false };

	// the wheel's covers: the games that have a texture now, and textures waiting to be destroyed
	std::vector<int> textured;
	std::vector<std::uint32_t> retired;
	double retireAge{ 0.0 };
	std::set<std::string> favourites;
	GameInfo info;
	unsigned libSeen{ 0 }; // the engine's list number the wheel's list was made from
	static constexpr int kKeep = 12; // covers kept each side of the wheel's centre
	int wantedFor{ -2 };             // the centre the covers were last asked for
	unsigned wantedSerial{ 0 };
	// the open card's extras (Wikipedia's words, a screenshot behind the card, the game's logo): held here, for the
	// engine's game extrasFor, and copied onto that game in the wheel's list each frame
	int extrasFor{ -1 };
	bool extrasInfoIn{ false };
	std::string extrasAbout, extrasReviews, extrasCredit;
	int extrasState{ 0 };
	std::uint32_t extrasBackdrop{ 0 }, extrasLogo{ 0 };
	float extrasBackdropAspect{ 0.75f }, extrasLogoAspect{ 0.25f };

	// getting a game ready
	int startingIndex{ -1 };
	cdie_job job{};
	std::string failTitle, failText;

	// the game's picture: two images, the newest frame and the one before it
	VideoSurface video[2];
	std::uint32_t videoTex[2]{ 0, 0 };
	unsigned texSerial[2]{ 0, 0 };
	int texW[2]{ 0, 0 }, texH[2]{ 0, 0 };
	int cur{ 0 }; // the image holding the newest frame
	bool videoReady{ false };
	// the picture filter: each frame filtered into an image the size it is shown at (Picture::normal: none)
	PictureFilter picture;
	std::uint32_t pictureTex[2]{ 0, 0 };
	Picture pictureMode{ Picture::normal };
	int pictureLogged{ -1 }; // the mode whose first result was logged
	struct Filtered { unsigned serial; int key[7]; };
	Filtered filtered[2]{};
	int frameDbl[2]{ 0, 0 }; // each image's doubled pixels (doubledPixels)

	// the in-game menu
	enum class Row { resume, save, load, disc, setting, picture, fit, reset, list, quit };
	struct MenuRow { Row kind; int setting; std::string label, value; };
	bool menuOpen{ false };
	uint32_t heldOver{ 0 }; // buttons still down from the menu (Cross on Resume): kept from the game until let go
	SettingsScreen setup;
	Controls controls;            // OPTIONS (a short press) on the wheel
	hui::gfx::DrawList setupList;
	bool optionsDown{ false };          // OPTIONS went down on the wheel ...
	float optionsFor{ 0 };              // ... this long ago: a short press opens the settings, a long one closes the app
	int menuSel{ 0 };
	int slot{ 1 };
	int discPick{ 0 };
	std::vector<MenuRow> rows;
	std::string menuMsg;
	double menuMsgAt{ -1e9 };

	VulkanExample() : KitExample()
	{
		title = "Seedee5";
		name = "cdiwheel";
		// The kit draws the whole screen and owns the pad
		settings.overlay = false;
		ps5.ownOverlay = true;
		defaultClearColor = { { 0.0f, 0.0f, 0.0f, 1.0f } };
	}

	// The kit's sounds and the game's, on the console's one output (the audio thread calls this)
	static void fillAll(int16_t *out, int frames, void *user)
	{
		VulkanExample *self = static_cast<VulkanExample *>(user);
		self->kit.mixer.render(out, frames);
		if (self->gameSound.load()) {
			static int16_t game[4096 * 2];
			const int n = std::min(frames, 4096);
			cdie_audio(game, n);
			for (int i = 0; i < n * 2; i++) {
				const int v = static_cast<int>(out[i]) + static_cast<int>(game[i]);
				out[i] = static_cast<int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
			}
		}
	}

	void prepare() override
	{
		VulkanExampleBase::prepare();
		ps5ui::Kit::Options options;
		options.music = false; // the games have their own
		prepareKit(options);
		choices.music_volume = 0;
		if (kit.audio) {
			audio_stop();
			kit.audio = audio_start(fillAll, this);
		}

		const std::string data = dataPath(nullptr), saves = dataPath("saves"), log = dataPath("saves/title_log.txt");
		mkdir(data.c_str(), 0777);
		mkdir(saves.c_str(), 0777);
		chmod(data.c_str(), 0777);
		chmod(saves.c_str(), 0777);
		g_logFile = std::fopen(log.c_str(), "w"); // each launch starts it again: copy it before relaunching
		if (g_logFile) {
			chmod(log.c_str(), 0666);
		}
		{
			char line[700];
			std::snprintf(line, sizeof line, "CDI: Seedee5 %s (built %s %s), data folder %s (over FTP: %s)", SEEDEE5_VERSION, __DATE__, __TIME__,
				data.c_str(), ftpPath(data).c_str());
			logLine(line);
		}
		importOldData(logLine);
		cdie_config cfg{};
		cfg.data_dir = data.c_str();
		cfg.log = logLine;
		cfg.notice = noticeLine;
		engineOk = cdie_init(&cfg) == 0;
		if (!engineOk) {
			engineError = cdie_last_error();
			std::cerr << "CDI: the engine could not start: " << engineError << std::endl;
		}
		if (engineOk && cdie_keep_note()[0]) {
			cdi::library().keepNote = cdie_keep_note();
			logLine((std::string("CDI: ") + cdie_keep_note()).c_str());
		}
		loadFavourites();
		info.load(dataPath("games_info.txt"));
		controls.load(dataPath("saves/controls.txt"));
		setup.controls = &controls;
		libSeen = engineOk ? cdie_lib_update() : 0;
		buildLibrary();
		cdi::jumpTo() = engineOk ? viewOfEngine(cdie_lib_last()) : -1;
		if (engineOk) {
			// a first launch (no games and no NAS yet), or no BIOS: the settings say what to do
			cdie_nas_cfg nas;
			cdie_nas_get(&nas);
			cdie_bios bios;
			cdie_bios_status(&bios);
			if ((cdie_lib_count() == 0 && !nas.enabled) || !bios.bios) {
				setup.show(cdie_lib_count());
			}
		}

		for (int i = 0; i < 2; i++) {
			if (!video[i].create(physicalDevice, device, queue, vulkanDevice->queueFamilyIndices.graphics)) {
				std::cerr << "CDI: could not make the video images" << std::endl;
				break;
			}
			videoTex[i] = kit.renderer.import_texture(video[i].view);
		}
		videoReady = videoTex[0] && videoTex[1];
		if (videoReady) {
			const VkImageView views[2] = { video[0].view, video[1].view };
			if (picture.create(physicalDevice, device, queue, vulkanDevice->queueFamilyIndices.graphics, views, VideoSurface::kWidth,
					VideoSurface::kHeight)) {
				for (int i = 0; i < 2; i++) {
					pictureTex[i] = kit.renderer.import_texture(picture.outView[i]);
				}
				const std::string line = "CDI: picture filters ready, up to " + std::to_string(picture.maxW) + "x" + std::to_string(picture.maxH) +
					(picture.why.empty() ? std::string() : " " + picture.why) +
					(pictureTex[0] && pictureTex[1] ? "" : ", but the kit did not take their images");
				logLine(line.c_str());
			} else {
				const std::string line = "CDI: the picture filters could not start: " + picture.why;
				logLine(line.c_str());
				picture.destroy();
			}
		}

		{ // the display, and whether HDR could be had: the colour spaces need VK_EXT_swapchain_colorspace (instance)
			// and HDR metadata VK_EXT_hdr_metadata (device); the swapchain uses what the template picked
			uint32_t n = 0;
			vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
			std::vector<VkExtensionProperties> ie(n);
			vkEnumerateInstanceExtensionProperties(nullptr, &n, ie.data());
			n = 0;
			vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &n, nullptr);
			std::vector<VkExtensionProperties> de(n);
			vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &n, de.data());
			auto has = [](const std::vector<VkExtensionProperties> &v, const char *name) {
				for (const auto &e : v) {
					if (std::strcmp(e.extensionName, name) == 0) {
						return true;
					}
				}
				return false;
			};
			char line[300];
			std::snprintf(line, sizeof line,
				"CDI: display %ux%u, format %d, colour space %d; HDR needs: VK_EXT_swapchain_colorspace %s, VK_EXT_hdr_metadata %s",
				width, height, static_cast<int>(swapChain.colorFormat), static_cast<int>(swapChain.colorSpace),
				has(ie, "VK_EXT_swapchain_colorspace") ? "offered" : "not offered", has(de, "VK_EXT_hdr_metadata") ? "offered" : "not offered");
			logLine(line);
		}
		{ // the logo at the top left of the wheel
			int lw = 0, lh = 0;
			uint8_t *rgba = cdie_decode_image(kSeedee5LogoPng, static_cast<int>(sizeof kSeedee5LogoPng), &lw, &lh);
			if (rgba) {
				cdi::library().logo = kit.renderer.create_texture(lw, lh, rgba);
				cdie_free(rgba);
			}
		}
		context = std::make_unique<hui::app::Context>(hui::app::Context{ kit.fonts, kit.catalog, telemetry, choices });
		screen = makeScreen(*context);
		screen->enter();
		kit.apply(choices);
		feedback.play(hui::audio::Cue::welcome);
		if (!engineOk) {
			failTitle = "The CD-i engine could not start";
			failText = engineError;
			mode = Mode::failed;
		}
		prepared = true;
	}

	// ---- the list ----

	static std::string upperExt(const std::string &path)
	{
		const size_t dot = path.rfind('.');
		std::string e = dot == std::string::npos ? std::string() : path.substr(dot + 1);
		for (char &c : e) {
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		}
		return e;
	}

	static int bucket(char letter)
	{
		return letter >= 'A' && letter <= 'Z' ? letter - 'A' + 1 : 0;
	}

	// "Burn:Cycle (Europe) (En,Fr)" -> title "Burn:Cycle", tags "Europe  ·  En,Fr"
	static void splitName(const std::string &name, std::string &titleOut, std::string &tags)
	{
		const size_t open = name.find(" (");
		const size_t square = name.find(" [");
		const size_t cut = std::min(open, square);
		titleOut = cut == std::string::npos ? name : name.substr(0, cut);
		tags.clear();
		for (size_t i = 0; i < name.size(); i++) {
			if (name[i] == '(' || name[i] == '[') {
				const char close = name[i] == '(' ? ')' : ']';
				const size_t end = name.find(close, i + 1);
				if (end == std::string::npos) {
					break;
				}
				if (!tags.empty()) {
					tags += "  \xC2\xB7  ";
				}
				tags += name.substr(i + 1, end - i - 1);
				i = end;
			}
		}
	}

	void retireTextures()
	{
		for (const int index : textured) {
			auto &games = cdi::library().games;
			if (index >= 0 && index < static_cast<int>(games.size()) && games[static_cast<size_t>(index)].cover) {
				retired.push_back(games[static_cast<size_t>(index)].cover);
				games[static_cast<size_t>(index)].cover = 0;
				cdie_cover_forget(games[static_cast<size_t>(index)].engine); // wanted again when it shows again
			}
		}
		textured.clear();
	}

	// What the wheel shows (the touch pad on the wheel moves to the next): every game, the favourites, the ones
	// played most recently first, the ones on the console, the ones on the NAS
	enum class View { all, favourites, recent, console, nas, count };
	View view{ View::all };
	std::vector<cdi::Game> allGames; // every game in the engine's list, in its order
	std::vector<int> viewOf;         // engine index -> position on the wheel, or -1

	static const char *viewName(View v)
	{
		switch (v) {
		case View::favourites: return "FAVOURITES";
		case View::recent: return "RECENTLY PLAYED";
		case View::console: return "ON THIS CONSOLE";
		case View::nas: return "ON THE NAS";
		default: return "";
		}
	}

	// The wheel's list, made again from the engine's (the old textures go: the indexes changed)
	void buildLibrary()
	{
		allGames.clear();
		const int n = engineOk ? cdie_lib_count() : 0;
		allGames.reserve(static_cast<size_t>(n));
		for (int i = 0; i < n; i++) {
			cdie_game g;
			cdi::Game game;
			if (cdie_lib_game(i, &g) != 0) {
				game.name = "?";
				game.engine = i;
				allGames.push_back(std::move(game));
				continue;
			}
			game.engine = i;
			game.name = g.name;
			game.path = g.path;
			game.letter = g.letter;
			game.discs = g.discs;
			game.here = g.here != 0;
			game.onNas = g.source == CDIE_SRC_NAS;
			game.source = g.source == CDIE_SRC_NAS ? "NAS" : g.source == CDIE_SRC_DRIVE ? std::string(g.drive) : "Console";
			game.format = upperExt(game.path);
			std::string tags;
			splitName(game.name, game.title, tags);
			game.meta = tags;
			const std::string where = g.source == CDIE_SRC_NAS ? (game.here ? "NAS (copied)" : "on the NAS")
				: g.source == CDIE_SRC_DRIVE ? std::string("on ") + g.drive : std::string();
			if (!where.empty()) {
				game.meta += (game.meta.empty() ? "" : "  \xC2\xB7  ") + where;
			}
			if (g.discs > 1) {
				game.meta += (game.meta.empty() ? "" : "  \xC2\xB7  ") + std::to_string(g.discs) + " discs";
			}
			game.favorite = favourites.count(game.path) != 0;
			refreshGame(game);
			info.fill(game);
			allGames.push_back(std::move(game));
		}
		applyView();
	}

	// what changes as the game is played: Continue, the time played
	void refreshGame(cdi::Game &game)
	{
		char when[48];
		game.resume = cdie_continue_info(game.engine, when, sizeof when) ? when : "";
		long secs = 0, last = 0;
		cdie_play_info(game.engine, &secs, &last);
		game.playSecs = secs;
		game.lastPlayed = last;
		game.played.clear();
		if (secs >= 60) {
			char t[96];
			if (secs >= 3600) {
				std::snprintf(t, sizeof t, "played %ld h %02ld min", secs / 3600, secs / 60 % 60);
			} else {
				std::snprintf(t, sizeof t, "played %ld min", secs / 60);
			}
			game.played = t;
			if (last > 0) {
				struct tm tmv;
				time_t tt = last;
				localtime_r(&tt, &tmv);
				char d[32];
				std::strftime(d, sizeof d, "%d %b %Y", &tmv);
				game.played += std::string(", last on ") + d;
			}
		}
	}

	// The wheel's games from allGames, as the view asks
	void applyView()
	{
		cdi::Library &lib = cdi::library();
		// keep each game's cover texture with the game (textures belong to the shown list)
		retireTextures();
		lib.games.clear();
		lib.roomFor = ftpPath(dataPath("roms"));
		viewOf.assign(allGames.size(), -1);
		std::vector<const cdi::Game *> pick;
		for (const cdi::Game &g : allGames) {
			bool in = true;
			switch (view) {
			case View::favourites: in = g.favorite; break;
			case View::recent: in = g.lastPlayed > 0; break;
			case View::console: in = !g.onNas || g.here; break;
			case View::nas: in = g.onNas; break;
			default: break;
			}
			if (in && g.name != "?") {
				pick.push_back(&g);
			}
		}
		if (view == View::recent) {
			std::stable_sort(pick.begin(), pick.end(), [](const cdi::Game *a, const cdi::Game *b) { return a->lastPlayed > b->lastPlayed; });
		}
		for (const cdi::Game *g : pick) {
			viewOf[static_cast<size_t>(g->engine)] = static_cast<int>(lib.games.size());
			lib.games.push_back(*g);
			lib.games.back().cover = 0;
		}
		lib.view = viewName(view);
		lib.emptyText = view == View::favourites ? "Triangle on a game makes it a favourite."
			: view == View::recent ? "Games show here once they have been played."
			: view == View::nas ? "No games on the NAS: set one up in Settings (OPTIONS)."
			: view == View::console ? "No games on the console: copy them over FTP, or start one from the NAS." : "";
		// the first game of each letter; a letter with none points at the next one's (a sorted view has no letters)
		const int count = static_cast<int>(lib.games.size());
		int next = count;
		for (int b = 26; b >= 0; b--) {
			int first = -1;
			for (int i = 0; i < count && view != View::recent; i++) {
				if (bucket(lib.games[static_cast<size_t>(i)].letter) == b) {
					first = i;
					break;
				}
			}
			lib.letters[b] = first >= 0 ? first : next;
			next = lib.letters[b];
		}
		lib.serial++;
	}

	int engineOfView(int v) const
	{
		const auto &games = cdi::library().games;
		return v >= 0 && v < static_cast<int>(games.size()) ? games[static_cast<size_t>(v)].engine : -1;
	}

	int viewOfEngine(int e) const
	{
		return e >= 0 && e < static_cast<int>(viewOf.size()) ? viewOf[static_cast<size_t>(e)] : -1;
	}

	void nextView()
	{
		const int c = cdi::centre();
		const int keepEngine = engineOfView(c);
		view = static_cast<View>((static_cast<int>(view) + 1) % static_cast<int>(View::count));
		applyView();
		const int at = viewOfEngine(keepEngine);
		cdi::jumpTo() = at >= 0 ? at : 0;
		screen->enter();
		feedback.play(hui::audio::Cue::tab);
	}

	void loadFavourites()
	{
		favourites.clear();
		FILE *f = std::fopen(dataPath("saves/favourites.txt").c_str(), "r");
		if (!f) {
			return;
		}
		char line[1200];
		while (std::fgets(line, sizeof line, f)) {
			line[std::strcspn(line, "\r\n")] = 0;
			if (line[0]) {
				favourites.insert(line);
			}
		}
		std::fclose(f);
	}

	void saveFavourites()
	{
		const std::string path = dataPath("saves/favourites.txt");
		FILE *f = std::fopen(path.c_str(), "w");
		if (!f) {
			return;
		}
		for (const std::string &p : favourites) {
			std::fprintf(f, "%s\n", p.c_str());
		}
		std::fclose(f);
		chmod(path.c_str(), 0666);
	}

	// The picture's average colour is the cover's body tone; the design eases its backdrop toward it
	static void paletteFrom(const uint8_t *rgba, int w, int h, cdi::Game &game)
	{
		double r = 0.0, g = 0.0, b = 0.0;
		long samples = 0;
		for (int y = 0; y < h; y += 8) {
			for (int x = 0; x < w; x += 8) {
				const uint8_t *p = rgba + 4 * (static_cast<size_t>(y) * w + x);
				r += p[0];
				g += p[1];
				b += p[2];
				samples++;
			}
		}
		if (samples == 0) {
			return;
		}
		const hui::gfx::Color mid{ static_cast<float>(r / samples / 255.0), static_cast<float>(g / samples / 255.0),
			static_cast<float>(b / samples / 255.0), 1.0f };
		const float peak = std::max({ mid.r, mid.g, mid.b, 0.05f });
		game.mid = mid;
		game.dark = hui::gfx::mix(mid, hui::gfx::Color{ 0.02f, 0.02f, 0.03f, 1.0f }, 0.65f);
		game.accent = hui::gfx::mix(hui::gfx::Color{ mid.r / peak, mid.g / peak, mid.b / peak, 1.0f },
			hui::gfx::Color{ 1.0f, 1.0f, 1.0f, 1.0f }, 0.15f);
	}

	// Once a frame on the wheel: the list, its status lines, the covers near the centre, the design's requests
	void housekeeping(double dt)
	{
		if (!engineOk) {
			return;
		}
		cdi::Library &lib = cdi::library();
		const unsigned serial = cdie_lib_update();
		if (serial != libSeen) {
			libSeen = serial;
			// keep the wheel on the same game
			std::string keep;
			const int c = cdi::centre();
			if (c >= 0 && c < static_cast<int>(lib.games.size())) {
				keep = lib.games[static_cast<size_t>(c)].path;
			}
			const bool first = lib.games.empty();
			buildLibrary();
			if (!keep.empty()) {
				const int at = viewOfEngine(cdie_lib_find(keep.c_str()));
				cdi::jumpTo() = at >= 0 ? at : 0;
			} else if (first) {
				cdi::jumpTo() = viewOfEngine(cdie_lib_last());
			}
		}
		char nas[512], covers[256];
		lib.nasState = cdie_lib_status(nas, sizeof nas, covers, sizeof covers);
		lib.nas = nas;
		lib.covers = covers;

		// covers: the ones near the centre are read first; far ones go back
		// (the engine counts games in its own list; the wheel shows a view of it)
		const int centre = cdi::centre();
		if (centre != wantedFor || lib.serial != wantedSerial) {
			wantedFor = centre;
			wantedSerial = lib.serial;
			std::vector<int> want; // the centre's cover first, then outwards
			const int n = static_cast<int>(lib.games.size());
			if (centre >= 0 && centre < n) {
				want.push_back(lib.games[static_cast<size_t>(centre)].engine);
			}
			for (int d = 1; d <= kKeep; d++) {
				for (const int v : { centre - d, centre + d }) {
					if (v >= 0 && v < n) {
						want.push_back(lib.games[static_cast<size_t>(v)].engine);
					}
				}
			}
			cdie_cover_wanted(want.data(), static_cast<int>(want.size()), 512);
		}
		int engineIndex, w, h;
		uint8_t *rgba;
		while (cdie_cover_take(&engineIndex, &w, &h, &rgba)) {
			const int index = viewOfEngine(engineIndex);
			const bool wanted = index >= 0 && index < static_cast<int>(lib.games.size()) && std::abs(index - centre) <= kKeep + 4 &&
				!lib.games[static_cast<size_t>(index)].cover;
			if (wanted) {
				cdi::Game &game = lib.games[static_cast<size_t>(index)];
				game.cover = kit.renderer.create_texture(w, h, rgba);
				game.aspect = static_cast<float>(h) / static_cast<float>(w);
				paletteFrom(rgba, w, h, game);
				textured.push_back(index);
			} else {
				cdie_cover_forget(engineIndex);
			}
			cdie_free(rgba);
		}
		for (size_t i = 0; i < textured.size();) {
			const int t = textured[i];
			if (std::abs(t - centre) > kKeep + 6 && t >= 0 && t < static_cast<int>(lib.games.size())) {
				retired.push_back(lib.games[static_cast<size_t>(t)].cover);
				lib.games[static_cast<size_t>(t)].cover = 0;
				cdie_cover_forget(lib.games[static_cast<size_t>(t)].engine);
				textured.erase(textured.begin() + static_cast<std::ptrdiff_t>(i));
			} else {
				i++;
			}
		}
		updateExtras(lib);
		retireAge += dt;
		flushRetired(false);

		// the touch pad on the wheel: the next list (every game, favourites, recently played, console, NAS)
		if (!setup.open && !ps5.frameBudget && (ps5_pad().pressed & PAD_TOUCH_PAD)) {
			nextView();
		}

		// what the design asked for
		std::vector<int> &toggles = cdi::favouriteToggles();
		if (!toggles.empty()) {
			for (const int t : toggles) {
				if (t >= 0 && t < static_cast<int>(lib.games.size())) {
					cdi::Game &game = lib.games[static_cast<size_t>(t)];
					game.favorite = !game.favorite;
					if (game.engine >= 0 && game.engine < static_cast<int>(allGames.size())) {
						allGames[static_cast<size_t>(game.engine)].favorite = game.favorite;
					}
					if (game.favorite) {
						favourites.insert(game.path);
					} else {
						favourites.erase(game.path);
					}
				}
			}
			toggles.clear();
			saveFavourites();
		}
		if (cdi::rescanRequest()) {
			cdi::rescanRequest() = false;
			cdie_lib_rescan();
		}
		const int play = cdi::playRequest();
		if (play >= 0) {
			cdi::playRequest() = -1;
			startGame(engineOfView(play)); // (the wheel's position: the engine wants its own index)
		}
	}

	void dropExtras()
	{
		for (std::uint32_t *t : { &extrasBackdrop, &extrasLogo }) {
			if (*t) {
				retired.push_back(*t);
				*t = 0;
			}
		}
		extrasInfoIn = false;
		extrasAbout.clear();
		extrasReviews.clear();
		extrasCredit.clear();
		extrasState = 0;
	}

	// The open card's extras: asked for when a card opens, dropped when it closes, put on the game as they come
	void updateExtras(cdi::Library &lib)
	{
		const int open = cdi::openCard();
		const int n = static_cast<int>(lib.games.size());
		const int engineIndex = open >= 0 && open < n ? lib.games[static_cast<size_t>(open)].engine : -1;
		if (engineIndex != extrasFor) {
			dropExtras();
			extrasFor = engineIndex;
			if (engineIndex >= 0) {
				const std::string title = info.articleTitle(lib.games[static_cast<size_t>(open)].title);
				cdie_extras_want(engineIndex, title.c_str());
			} else {
				cdie_extras_want(-1, nullptr);
			}
		}
		int index, kind, w, h;
		uint8_t *rgba;
		while (cdie_extras_take(&index, &kind, &w, &h, &rgba)) {
			if (index == extrasFor && w > 0 && h > 0) {
				std::uint32_t &tex = kind ? extrasLogo : extrasBackdrop;
				if (tex) {
					retired.push_back(tex);
				}
				tex = kit.renderer.create_texture(w, h, rgba);
				(kind ? extrasLogoAspect : extrasBackdropAspect) = static_cast<float>(h) / static_cast<float>(w);
			}
			cdie_free(rgba);
		}
		if (extrasFor >= 0 && !extrasInfoIn) {
			cdie_extras_info got{};
			if (cdie_extras_info_get(extrasFor, &got)) {
				extrasInfoIn = true;
				extrasAbout = got.about;
				extrasReviews = got.reviews;
				extrasCredit = got.about[0] ? got.source : "";
				extrasState = got.about[0] ? 1 : 2;
			}
		}
		for (int i = 0; i < n; i++) {
			cdi::Game &g = lib.games[static_cast<size_t>(i)];
			const bool mine = i == open && g.engine == extrasFor && extrasFor >= 0;
			g.about = mine ? extrasAbout : std::string();
			g.reviews = mine ? extrasReviews : std::string();
			g.credit = mine ? extrasCredit : std::string();
			g.aboutState = mine ? extrasState : 0;
			g.backdrop = mine ? extrasBackdrop : 0;
			g.backdropAspect = extrasBackdropAspect;
			g.titleLogo = mine ? extrasLogo : 0;
			g.titleLogoAspect = extrasLogoAspect;
		}
	}

	// Textures go in batches, once the device is idle
	void flushRetired(bool force)
	{
		if (retired.empty() || (!force && retired.size() < 6 && retireAge < 1.0)) {
			return;
		}
		if (device) {
			vkDeviceWaitIdle(device);
		}
		for (const std::uint32_t id : retired) {
			if (id) {
				kit.renderer.destroy_texture(id);
			}
		}
		retired.clear();
		retireAge = 0.0;
	}

	// ---- starting a game ----

	bool continuing{ false }; // the game asked for starts from its automatic save

	void startGame(int index)
	{
		continuing = cdi::continueRequest();
		cdi::continueRequest() = false;
		cdi::playing() = true;
		startingIndex = index;
		std::memset(&job, 0, sizeof job);
		if (cdie_prepare(index) != 0) {
			fail("Could not start the game", "it is not in the list any more");
			return;
		}
		mode = Mode::preparing;
	}

	void fail(const std::string &heading, const std::string &why)
	{
		failTitle = heading;
		failText = why;
		mode = Mode::failed;
		cdi::playing() = false;
		std::cerr << "CDI: " << heading << ": " << why << std::endl;
	}

	// While the game is copied or unpacked; once it is ready, the core loads it (a second or two, waited for here)
	void updatePreparing(const hui::InputFrame &input)
	{
		const int state = cdie_prepare_poll(&job);
		if (state == 0) {
			if (input.is_pressed(hui::Action::back)) {
				cdie_prepare_cancel();
			}
			return;
		}
		if (state == 1) {
			cdie_continue_next(continuing ? 1 : 0);
			if (cdie_start() != 0) {
				fail("Could not start the game", cdie_last_error());
				return;
			}
			mode = Mode::playing;
			menuOpen = false;
			texSerial[0] = texSerial[1] = 0;
			filtered[0] = filtered[1] = Filtered{};
			pictureMode = loadPicture(cdie_game_name());
			gameSound = true;
			return;
		}
		if (state == 2) {
			fail("Could not get the game ready", job.why);
			return;
		}
		// cancelled
		mode = Mode::list;
		cdi::playing() = false;
	}

	void backToList()
	{
		gameSound = false;
		cdie_stop(); // the automatic save for Continue is made here
		menuOpen = false;
		mode = Mode::list;
		cdi::playing() = false;
		// Continue and the time played changed: in every game list (startingIndex is the engine's)
		if (startingIndex >= 0 && startingIndex < static_cast<int>(allGames.size())) {
			refreshGame(allGames[static_cast<size_t>(startingIndex)]);
			const int v = viewOfEngine(startingIndex);
			auto &games = cdi::library().games;
			if (v >= 0 && v < static_cast<int>(games.size())) {
				refreshGame(games[static_cast<size_t>(v)]);
			}
		}
		screen->enter();
	}

	// ---- the game ----

	static uint32_t mapButtons(uint32_t held)
	{
		static const struct { uint32_t from, to; } map[] = {
			{ PAD_CROSS, CDIE_PAD_CROSS }, { PAD_CIRCLE, CDIE_PAD_CIRCLE }, { PAD_SQUARE, CDIE_PAD_SQUARE },
			{ PAD_TRIANGLE, CDIE_PAD_TRIANGLE }, { PAD_L1, CDIE_PAD_L1 }, { PAD_R1, CDIE_PAD_R1 },
			{ PAD_L3, CDIE_PAD_L3 }, { PAD_R3, CDIE_PAD_R3 }, { PAD_OPTIONS, CDIE_PAD_OPTIONS },
			{ PAD_TOUCH_PAD, CDIE_PAD_TOUCH }, { PAD_UP, CDIE_PAD_UP }, { PAD_DOWN, CDIE_PAD_DOWN },
			{ PAD_LEFT, CDIE_PAD_LEFT }, { PAD_RIGHT, CDIE_PAD_RIGHT },
		};
		uint32_t out = 0;
		for (const auto &m : map) {
			if (held & m.from) {
				out |= m.to;
			}
		}
		return out;
	}

	void passPad()
	{
		const struct pad &p = ps5_pad();
		cdie_pad out{};
		if (!menuOpen && !ps5.frameBudget) {
			heldOver &= p.held; // each button held over from the menu reaches the game once it has been let go
			out.buttons = mapButtons(p.held & ~heldOver);
			auto axis = [](float v) { return static_cast<int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767.0f); };
			out.lx = axis(p.left_x);
			out.ly = axis(p.left_y);
			out.rx = axis(p.right_x);
			out.ry = axis(p.right_y);
			out.l2 = static_cast<uint8_t>(std::clamp(p.l2, 0.0f, 1.0f) * 255.0f);
			out.r2 = static_cast<uint8_t>(std::clamp(p.r2, 0.0f, 1.0f) * 255.0f);
			controls.apply(out, mapButtons); // a second controller
		}
		cdie_input(&out);
	}

	// The newest frames into the two images: a new frame goes into the image that does not hold the newest one,
	// so the previous frame stays where it is for the blend
	void takeFrames()
	{
		if (!videoReady) {
			return;
		}
		cdie_image a, b;
		const int n = cdie_video_lock(&a, &b);
		if (n >= 1 && a.serial != texSerial[cur] && a.w <= VideoSurface::kWidth && a.h <= VideoSurface::kHeight) {
			const int other = cur ^ 1;
			if (n == 2 && texSerial[cur] != b.serial && b.w == a.w && b.h == a.h) {
				// both are new to us (a frame was missed): the previous one too
				video[cur].upload(b.rgba, b.w, b.h, b.pitch);
				frameDbl[cur] = doubledPixels(b.rgba, b.w, b.h, b.pitch);
				texSerial[cur] = b.serial;
				texW[cur] = b.w;
				texH[cur] = b.h;
			}
			video[other].upload(a.rgba, a.w, a.h, a.pitch);
			frameDbl[other] = doubledPixels(a.rgba, a.w, a.h, a.pitch);
			texSerial[other] = a.serial;
			texW[other] = a.w;
			texH[other] = a.h;
			cur = other;
		}
		cdie_video_unlock();
	}

	// Is every pixel of this frame two columns wide (bit 0) or two rows tall (bit 1)? A 384-pixel CD-i picture comes
	// in a 768-wide frame. Checked on a spread of rows and columns; the filters then work on the real pixels.
	static int doubledPixels(const uint8_t *rgba, int w, int h, int pitch)
	{
		if (!rgba || w < 64 || h < 64) {
			return 0;
		}
		int dbl = 0;
		bool cols = (w % 2) == 0, rows = (h % 2) == 0;
		int colsDiffer = 0, rowsDiffer = 0;
		for (int k = 1; k < 32 && (cols || rows); k++) {
			const int y = (h * k / 32) & ~1;
			const uint32_t *r0 = reinterpret_cast<const uint32_t *>(rgba + static_cast<size_t>(y) * pitch);
			const uint32_t *r1 = reinterpret_cast<const uint32_t *>(rgba + static_cast<size_t>(y + 1) * pitch);
			for (int x = 0; x + 1 < w; x += 2) {
				if (r0[x] != r0[x + 1]) {
					colsDiffer++;
				}
				if (r0[x] != r1[x] || r0[x + 1] != r1[x + 1]) {
					rowsDiffer++;
				}
			}
		}
		// a black screen is "doubled" either way: only a picture with something in it decides
		if (cols && colsDiffer == 0) {
			dbl |= 1;
		}
		if (rows && rowsDiffer == 0) {
			dbl |= 2;
		}
		return dbl;
	}

	// The fast start (cdie_booting): the CD-i player's own start-up runs unseen; this card shows meanwhile
	void drawBooting(hui::gfx::DrawList &list, double now)
	{
		const hui::gfx::Color white{ 1, 1, 1, 1 };
		const hui::gfx::Color accent{ 0.55f, 0.75f, 1.0f, 1.0f };
		const hui::gfx::Rect r{ 460.0f, 380.0f, 1000.0f, 300.0f };
		drawPanel(list, r);
		hui::ui::text(list, kit.fonts.semibold, "STARTING", r.x + 48.0f, r.y + 70.0f, 18, accent, hui::gfx::Align::left, 3.0f);
		hui::ui::text(list, kit.fonts.display, kit.fonts.display.font->fit(cdie_game_name(), 40, r.w - 96.0f), r.x + 48.0f, r.y + 128.0f, 40, white);
		// a bar with a light running along it: how long the start takes is not known beforehand
		const hui::gfx::Rect bar{ r.x + 48.0f, r.y + 180.0f, r.w - 96.0f, 14.0f };
		list.rounded_rect(bar, 7.0f, white.with_alpha(0.14f));
		const float run = static_cast<float>(std::fmod(now / 1400.0, 1.0));
		const float w = bar.w * 0.28f;
		const float x0 = bar.x + (bar.w + w) * run - w;
		const float a = std::max(bar.x, x0), b = std::min(bar.x + bar.w, x0 + w);
		if (b - a > 14.0f) {
			list.rounded_rect({ a, bar.y, b - a, bar.h }, 7.0f, accent);
		}
		hui::ui::text(list, kit.fonts.regular, "Skipping the CD-i player's own start-up", r.x + 48.0f, r.y + 240.0f, 22, white.with_alpha(0.6f));
		const hui::ui::Hint hints[] = { { hui::ui::Button::cross, "Show it now" } };
		hui::ui::draw_hints(list, kit.fonts, hui::ui::GlyphStyle::dark(), hints, 1, 1824, true);
	}

	void drawGame(hui::gfx::DrawList &list, double now)
	{
		if (cdie_booting()) {
			drawBooting(list, now);
			return;
		}
		if (!videoReady || !texSerial[cur]) {
			return;
		}
		const float blend = cdie_video_blend(now);
		cdie_view v;
		cdie_video_view(texW[cur], texH[cur], &v);
		const hui::gfx::Rect dst = v.wide ? hui::gfx::Rect{ 0.0f, 0.0f, 1920.0f, 1080.0f } : hui::gfx::Rect{ 240.0f, 0.0f, 1440.0f, 1080.0f };
		const hui::gfx::Rect uv{ static_cast<float>(v.x) / VideoSurface::kWidth, static_cast<float>(v.y) / VideoSurface::kHeight,
			static_cast<float>(v.w) / VideoSurface::kWidth, static_cast<float>(v.h) / VideoSurface::kHeight };
		const int prev = cur ^ 1;
		const bool havePrev = texSerial[prev] && texW[prev] == texW[cur] && texH[prev] == texH[cur];
		// a picture filter: each image filtered once (again when the crop, the size or the filter changes), and the
		// filtered image drawn 1:1
		const std::uint32_t *tex = videoTex;
		hui::gfx::Rect useUv = uv;
		if (pictureMode != Picture::normal && picture.ready && pictureTex[0] && pictureTex[1]) {
			// at the screen's own pixels: the kit draws in 1920x1080 units, the console's screen is 3840x2160
			const float px = std::max(1.0f, static_cast<float>(width) / 1920.0f);
			const int ow = static_cast<int>(dst.w * px + 0.5f), oh = static_cast<int>(dst.h * px + 0.5f);
			const int rect[4] = { v.x, v.y, v.w, v.h };
			bool ok = true;
			for (const int slot : { cur, prev }) {
				if (slot == prev && !havePrev) {
					continue;
				}
				const int key[7] = { v.x, v.y, v.w, v.h, ow, oh, static_cast<int>(pictureMode) * 4 + frameDbl[slot] };
				Filtered &f = filtered[slot];
				if (f.serial != texSerial[slot] || std::memcmp(f.key, key, sizeof key) != 0) {
					if (picture.run(slot, pictureMode, rect, frameDbl[slot], ow, oh)) {
						f.serial = texSerial[slot];
						std::memcpy(f.key, key, sizeof key);
					} else {
						ok = false;
					}
				}
			}
			if (static_cast<int>(pictureMode) != pictureLogged) {
				pictureLogged = static_cast<int>(pictureMode);
				char line[200];
				std::snprintf(line, sizeof line, "CDI: picture %s: %s (%dx%d from %dx%d%s%s)", pictureName(pictureMode),
					ok ? "filtered" : "FAILED to filter: shown unfiltered", ow, oh, v.w, v.h,
					(frameDbl[cur] & 1) ? ", pixels two columns wide" : "", (frameDbl[cur] & 2) ? ", lines two rows tall" : "");
				logLine(line);
			}
			if (ok) {
				tex = pictureTex;
				useUv = hui::gfx::Rect{ 0.0f, 0.0f, static_cast<float>(std::min(ow, picture.maxW)) / static_cast<float>(picture.maxW),
					static_cast<float>(std::min(oh, picture.maxH)) / static_cast<float>(picture.maxH) };
			}
		}
		if (havePrev && blend < 0.999f) {
			list.image(tex[prev], dst, useUv, hui::gfx::Color{ 1.0f, 1.0f, 1.0f, 1.0f }, 0.0f);
			list.image(tex[cur], dst, useUv, hui::gfx::Color{ 1.0f, 1.0f, 1.0f, blend }, 0.0f);
		} else {
			list.image(tex[cur], dst, useUv, hui::gfx::Color{ 1.0f, 1.0f, 1.0f, 1.0f }, 0.0f);
		}
		if (cdie_rewinding()) { // a small mark while going back
			const hui::gfx::Rect pill{ 64.0f, 56.0f, 230.0f, 64.0f };
			list.rounded_rect(pill, 32.0f, hui::gfx::Color{ 0.02f, 0.02f, 0.04f, 0.78f });
			for (int k = 0; k < 2; k++) { // two triangles pointing back
				const float x = pill.x + 30.0f + k * 22.0f, cy = pill.y + 32.0f;
				for (int r = 0; r < 18; r++) {
					const float h = 18.0f - static_cast<float>(r);
					list.rounded_rect({ x + static_cast<float>(17 - r), cy - h, 1.2f, 2.0f * h }, 0.0f, hui::gfx::Color{ 1, 1, 1, 0.95f });
				}
			}
			hui::ui::text(list, kit.fonts.semibold, "Rewind", pill.x + 96.0f, pill.y + 42.0f, 26, hui::gfx::Color{ 1, 1, 1, 0.95f });
		}
	}

	// The picture filter of a game: saves/picture/<game>.txt, else the last one chosen (saves/picture.txt)
	static std::string pictureFile(const char *game)
	{
		return game && *game ? dataPath("saves/picture") + "/" + game + ".txt" : dataPath("saves/picture.txt");
	}

	static Picture readPicture(const std::string &path, bool *found)
	{
		int v = -1;
		if (FILE *f = std::fopen(path.c_str(), "r")) {
			if (std::fscanf(f, "%d", &v) != 1) {
				v = -1;
			}
			std::fclose(f);
		}
		*found = v >= 0 && v < static_cast<int>(Picture::count);
		return *found ? static_cast<Picture>(v) : Picture::normal;
	}

	static Picture loadPicture(const char *game)
	{
		bool found = false;
		const Picture own = readPicture(pictureFile(game), &found);
		return found ? own : readPicture(pictureFile(nullptr), &found);
	}

	static void savePicture(const char *game, Picture p)
	{
		const std::string dir = dataPath("saves/picture");
		mkdir(dir.c_str(), 0777);
		chmod(dir.c_str(), 0777);
		for (const std::string &path : { pictureFile(game), pictureFile(nullptr) }) {
			if (FILE *f = std::fopen(path.c_str(), "w")) {
				std::fprintf(f, "%d\n", static_cast<int>(p));
				std::fclose(f);
				chmod(path.c_str(), 0666);
			}
		}
	}

	// ---- the in-game menu (touch pad) ----

	void buildMenu()
	{
		rows.clear();
		char info[64];
		rows.push_back({ Row::resume, -1, "Resume", "" });
		cdie_slot_info(slot, info, sizeof info);
		rows.push_back({ Row::save, -1, "Save state", "slot " + std::to_string(slot) + ": " + info });
		rows.push_back({ Row::load, -1, "Load state", "slot " + std::to_string(slot) + ": " + info });
		const int discs = cdie_disc_count();
		if (discs > 1) {
			rows.push_back({ Row::disc, -1, "Disc", "insert " + std::to_string(discPick + 1) + " of " + std::to_string(discs) +
				(discPick == cdie_disc_current() ? " (in)" : "") });
		}
		for (int i = 0; i < cdie_setting_count(); i++) {
			cdie_setting s;
			if (cdie_setting_get(i, &s) != 0) {
				continue;
			}
			std::string value = s.names ? std::string(s.names[s.value]) : std::to_string(s.value) + (s.unit ? s.unit : "");
			rows.push_back({ Row::setting, i, s.label, value });
		}
		// (always listed: when the filters could not start on this console it says so; title_log.txt says why)
		rows.push_back({ Row::picture, -1, "Picture", picture.ready ? pictureName(pictureMode) : "not available (see title_log.txt)" });
		rows.push_back({ Row::fit, -1, "Fit the crop to the picture", "" });
		rows.push_back({ Row::reset, -1, "Reset the game", "" });
		rows.push_back({ Row::list, -1, "Back to the game list", "" });
		rows.push_back({ Row::quit, -1, "Close Seedee5", "" });
		menuSel = std::clamp(menuSel, 0, static_cast<int>(rows.size()) - 1);
	}

	void openMenu()
	{
		cdie_pause(1);
		menuOpen = true;
		menuSel = 0;
		menuMsg.clear();
		discPick = cdie_disc_current();
		buildMenu();
		feedback.play(hui::audio::Cue::modal_open);
	}

	void closeMenu()
	{
		heldOver = ps5_pad().held; // the button that closed the menu is still down: not for the game
		menuOpen = false;
		cdie_pause(0);
		feedback.play(hui::audio::Cue::resume);
	}

	void stepPicture(int d)
	{
		if (!picture.ready) {
			return;
		}
		const int n = static_cast<int>(Picture::count);
		pictureMode = static_cast<Picture>((static_cast<int>(pictureMode) + d + n) % n);
		savePicture(cdie_game_name(), pictureMode);
	}

	void say(const char *msg)
	{
		menuMsg = msg;
		menuMsgAt = cdie_now_ms();
	}

	void updateMenu(const hui::InputFrame &input)
	{
		if (input.is_pressed(hui::Action::back) || input.is_pressed(hui::Action::touch)) {
			closeMenu();
			return;
		}
		const int n = static_cast<int>(rows.size());
		if (input.nav == hui::Direction::up) {
			menuSel = (menuSel + n - 1) % n;
			feedback.play(hui::audio::Cue::focus);
		} else if (input.nav == hui::Direction::down) {
			menuSel = (menuSel + 1) % n;
			feedback.play(hui::audio::Cue::focus);
		}
		const MenuRow row = rows[static_cast<size_t>(menuSel)];
		const int d = input.nav == hui::Direction::right ? 1 : input.nav == hui::Direction::left ? -1 : 0;
		char msg[300] = "";
		if (d) {
			switch (row.kind) {
			case Row::save:
			case Row::load:
				slot = (slot - 1 + d + 4) % 4 + 1;
				break;
			case Row::disc: {
				const int discs = cdie_disc_count();
				discPick = (discPick + d + discs) % discs;
				break;
			}
			case Row::setting: {
				cdie_setting s;
				if (cdie_setting_get(row.setting, &s) == 0) {
					cdie_setting_set(row.setting, s.value + d * (s.step ? s.step : 1));
				}
				break;
			}
			case Row::picture:
				stepPicture(d);
				break;
			default:
				break;
			}
			feedback.play(hui::audio::Cue::focus);
			buildMenu();
		}
		if (!input.is_pressed(hui::Action::confirm)) {
			return;
		}
		switch (row.kind) {
		case Row::resume:
			closeMenu();
			return;
		case Row::save:
			cdie_slot_save(slot, msg, sizeof msg);
			say(msg);
			break;
		case Row::load:
			if (cdie_slot_load(slot, msg, sizeof msg) == 0) {
				say(msg);
				closeMenu();
				return;
			}
			say(msg);
			break;
		case Row::disc:
			cdie_disc_insert(discPick, msg, sizeof msg);
			say(msg);
			break;
		case Row::setting: {
			cdie_setting s;
			if (cdie_setting_get(row.setting, &s) == 0 && s.names) { // Cross on a choice moves it on
				cdie_setting_set(row.setting, s.value + 1);
			}
			break;
		}
		case Row::picture:
			stepPicture(1);
			break;
		case Row::fit:
			cdie_fit_crop();
			say("the crop follows the picture now (the next frame)");
			break;
		case Row::reset:
			cdie_reset();
			closeMenu();
			return;
		case Row::list:
			backToList();
			return;
		case Row::quit:
			gameSound = false;
			cdie_stop();
			quit = true;
			return;
		}
		feedback.play(hui::audio::Cue::select);
		buildMenu();
	}

	void drawPanel(hui::gfx::DrawList &list, const hui::gfx::Rect &r)
	{
		list.shadow({ r.x, r.y + 14.0f, r.w, r.h }, 30.0f, 40.0f, hui::gfx::Color{ 0, 0, 0, 0.5f });
		list.rounded_rect(r, 30.0f, hui::gfx::Color{ 0.035f, 0.04f, 0.075f, 0.94f });
		list.bordered_rect(r, 30.0f, hui::gfx::Color{ 0, 0, 0, 0 }, 1.5f, hui::gfx::Color{ 1, 1, 1, 0.2f });
	}

	void drawMenu(hui::gfx::DrawList &list)
	{
		const hui::gfx::Color white{ 1, 1, 1, 1 };
		const float rowH = 52.0f;
		const float h = 150.0f + rowH * static_cast<float>(rows.size()) + 70.0f;
		const hui::gfx::Rect r{ 560.0f, std::max(20.0f, (1080.0f - h) * 0.5f), 800.0f, h };
		list.rounded_rect({ 0, 0, 1920.0f, 1080.0f }, 0, hui::gfx::Color{ 0, 0, 0, 0.45f });
		drawPanel(list, r);
		hui::ui::text(list, kit.fonts.semibold, "PAUSED", r.x + 40.0f, r.y + 56.0f, 16, white.with_alpha(0.55f), hui::gfx::Align::left, 3.0f);
		hui::ui::text(list, kit.fonts.display, kit.fonts.display.font->fit(cdie_game_name(), 40, r.w - 80.0f), r.x + 40.0f, r.y + 104.0f, 40, white);
		float y = r.y + 150.0f;
		for (size_t i = 0; i < rows.size(); i++) {
			const bool on = static_cast<int>(i) == menuSel;
			const hui::gfx::Rect box{ r.x + 24.0f, y, r.w - 48.0f, rowH - 6.0f };
			if (on) {
				list.rounded_rect(box, 16.0f, white);
			}
			const hui::gfx::Color ink = on ? hui::gfx::Color{ 0.04f, 0.05f, 0.09f, 1.0f } : white;
			hui::ui::text(list, kit.fonts.semibold, rows[i].label, box.x + 22.0f, box.y + 32.0f, 24, ink);
			if (!rows[i].value.empty()) {
				std::string v = on ? "<  " + rows[i].value + "  >" : rows[i].value;
				hui::ui::text(list, kit.fonts.regular, kit.fonts.regular.font->fit(v, 22, 380.0f), box.x + box.w - 22.0f, box.y + 31.0f, 22,
					ink.with_alpha(on ? 1.0f : 0.6f), hui::gfx::Align::right);
			}
			y += rowH;
		}
		if (!menuMsg.empty() && cdie_now_ms() - menuMsgAt < 6000.0) {
			hui::ui::text(list, kit.fonts.regular, kit.fonts.regular.font->fit(menuMsg, 22, r.w - 80.0f), r.x + 40.0f, y + 34.0f, 22,
				hui::gfx::Color{ 0.5f, 0.85f, 0.6f, 1.0f });
		}
		const hui::ui::Hint hints[] = { { hui::ui::Button::cross, "Choose" }, { hui::ui::Button::dpad, "Change" },
			{ hui::ui::Button::circle, "Back to the game" } };
		hui::ui::draw_hints(list, kit.fonts, hui::ui::GlyphStyle::dark(), hints, 3, 1824, true);
	}

	// ---- cards over the wheel: getting a game ready, and what went wrong ----

	void drawPreparing(hui::gfx::DrawList &list)
	{
		const hui::gfx::Color white{ 1, 1, 1, 1 };
		const hui::gfx::Rect r{ 460.0f, 330.0f, 1000.0f, 400.0f };
		list.rounded_rect({ 0, 0, 1920.0f, 1080.0f }, 0, hui::gfx::Color{ 0, 0, 0, 0.5f });
		drawPanel(list, r);
		const std::string gameName = startingIndex >= 0 && startingIndex < static_cast<int>(allGames.size())
			? allGames[static_cast<size_t>(startingIndex)].name : std::string();
		hui::ui::text(list, kit.fonts.semibold, hui::ui::upper(job.title[0] ? job.title : "Getting ready"), r.x + 48.0f, r.y + 70.0f, 18,
			hui::gfx::Color{ 0.55f, 0.75f, 1.0f, 1.0f }, hui::gfx::Align::left, 3.0f);
		hui::ui::text(list, kit.fonts.display, kit.fonts.display.font->fit(gameName, 40, r.w - 96.0f), r.x + 48.0f, r.y + 128.0f, 40, white);
		const hui::gfx::Rect bar{ r.x + 48.0f, r.y + 180.0f, r.w - 96.0f, 14.0f };
		list.rounded_rect(bar, 7.0f, white.with_alpha(0.14f));
		const float part = job.total ? static_cast<float>(static_cast<double>(job.done) / static_cast<double>(job.total)) : 0.0f;
		if (part > 0.0f) {
			list.rounded_rect({ bar.x, bar.y, std::max(14.0f, bar.w * std::min(part, 1.0f)), bar.h }, 7.0f, hui::gfx::Color{ 0.55f, 0.75f, 1.0f, 1.0f });
		}
		char text[200];
		std::snprintf(text, sizeof text, "%llu of %llu MB", static_cast<unsigned long long>(job.done >> 20),
			static_cast<unsigned long long>(job.total >> 20));
		hui::ui::text(list, kit.fonts.regular, text, r.x + 48.0f, r.y + 236.0f, 24, white.with_alpha(0.85f));
		if (job.mbps > 0.05) {
			const int secs = static_cast<int>(job.eta_s);
			std::snprintf(text, sizeof text, "%.1f MB/s  \xC2\xB7  about %d:%02d left", job.mbps, secs / 60, secs % 60);
			hui::ui::text(list, kit.fonts.regular, text, r.x + r.w - 48.0f, r.y + 236.0f, 24, white.with_alpha(0.6f), hui::gfx::Align::right);
		}
		hui::ui::text(list, kit.fonts.regular, kit.fonts.regular.font->fit(job.phase, 22, r.w - 96.0f), r.x + 48.0f, r.y + 284.0f, 22,
			white.with_alpha(0.6f));
		if (job.resumed) {
			hui::ui::text(list, kit.fonts.regular, "Part of this game was already here", r.x + 48.0f, r.y + 320.0f, 22, white.with_alpha(0.5f));
		}
		const hui::ui::Hint hints[] = { { hui::ui::Button::circle, "Cancel" } };
		hui::ui::draw_hints(list, kit.fonts, hui::ui::GlyphStyle::dark(), hints, 1, 1824, true);
	}

	void drawFailed(hui::gfx::DrawList &list)
	{
		const hui::gfx::Color white{ 1, 1, 1, 1 };
		const hui::gfx::Rect r{ 460.0f, 330.0f, 1000.0f, 360.0f };
		list.rounded_rect({ 0, 0, 1920.0f, 1080.0f }, 0, hui::gfx::Color{ 0, 0, 0, 0.5f });
		drawPanel(list, r);
		hui::ui::text(list, kit.fonts.display, kit.fonts.display.font->fit(failTitle, 40, r.w - 96.0f), r.x + 48.0f, r.y + 90.0f, 40,
			hui::gfx::Color{ 0.95f, 0.65f, 0.35f, 1.0f });
		hui::ui::paragraph(list, kit.fonts.regular, failText, r.x + 48.0f, r.y + 150.0f, 26, r.w - 96.0f, 38, white.with_alpha(0.85f), 4);
		hui::ui::text(list, kit.fonts.regular, "The log is " + ftpPath(dataPath("saves/title_log.txt")) + " (over FTP)", r.x + 48.0f, r.y + 320.0f, 20,
			white.with_alpha(0.45f));
		const hui::ui::Hint hints[] = { { hui::ui::Button::cross, "Back to the list" } };
		hui::ui::draw_hints(list, kit.fonts, hui::ui::GlyphStyle::dark(), hints, 1, 1824, true);
	}

	// The engine's newest news, for a few seconds at the top
	void drawNotice(hui::gfx::DrawList &list)
	{
		std::string text;
		double at;
		{
			std::lock_guard<std::mutex> hold(g_noticeMutex);
			text = g_notice;
			at = g_noticeAt;
		}
		const double age = cdie_now_ms() - at;
		if (text.empty() || age > 4000.0 || mode == Mode::list) {
			return;
		}
		const float a = static_cast<float>(std::min(1.0, (4000.0 - age) / 500.0));
		const std::string shown = kit.fonts.regular.font->fit(text, 22, 1400.0f);
		const float w = kit.fonts.regular.measure(shown, 22) + 56.0f;
		const hui::gfx::Rect r{ 960.0f - w * 0.5f, 28.0f, w, 52.0f };
		list.rounded_rect(r, 26.0f, hui::gfx::Color{ 0.035f, 0.04f, 0.075f, 0.85f * a });
		hui::ui::text(list, kit.fonts.regular, shown, 960.0f, r.y + 34.0f, 22, hui::gfx::Color{ 1, 1, 1, a }, hui::gfx::Align::center);
	}

	// ---- the frame ----

	// Holding OPTIONS for a second on the wheel closes the title; a press is the design's
	void holdOptions(float dt)
	{
		leaving.label = ps5.optionsEnds ? "Back to the menu" : "Close Seedee5";
		const bool held = !ps5.frameBudget && !setup.open && (ps5_pad().held & PAD_OPTIONS) != 0;
		if (held) {
			if (!optionsDown && (ps5_pad().pressed & PAD_OPTIONS)) {
				optionsDown = true; // only a press that began on the wheel counts (not the one that closed the settings)
				optionsFor = 0;
			}
			optionsFor += dt;
		} else if (optionsDown) {
			optionsDown = false;
			if (optionsFor < 0.5f && mode == Mode::list) {
				setup.show(cdie_lib_count());
				feedback.play(hui::audio::Cue::open);
			}
		}
		if (leaving.update(dt, held && optionsDown)) {
			quit = true;
		}
	}

	void updateTelemetry()
	{
		if (benchmark.active) {
			telemetry.push(16.7f);
			telemetry.fps = 59.9f;
			telemetry.average_ms = 16.7f;
		} else {
			telemetry.push(frameTimer * 1000.0f);
		}
		telemetry.draw_calls = kit.renderer.last_draw_calls();
		telemetry.instances = kit.renderer.last_instances();
	}

	void renderPlaying(float dt)
	{
		hui::InputFrame input = kit.input();
		if (ps5.frameBudget) {
			input = hui::InputFrame{};
			input.connected = true;
		}
		if (!cdie_running()) {
			backToList();
			return;
		}
		passPad();
		// L2 held: rewind (when the menu's Rewind setting is on; the engine ignores it otherwise)
		cdie_rewind(!menuOpen && !ps5.frameBudget && ps5_pad().l2 > 0.5f);
		if (menuOpen) {
			updateMenu(input);
		} else if (ps5_pad().pressed & PAD_TOUCH_PAD) {
			openMenu();
		} else if (cdie_booting() && (ps5_pad().pressed & PAD_CROSS)) {
			cdie_boot_skip(); // show the game now, wherever its start has got to
		}
		if (mode != Mode::playing) {
			return; // back to the list: the caller draws the wheel in this frame
		}
		takeFrames();
		kit.play(feedback, hui::audio::SoundSet::glass, choices.haptics);
		feedback.clear();
		kit.tick(dt);
		gameList.clear();
		drawGame(gameList, cdie_now_ms());
		menuList.clear();
		if (menuOpen) {
			drawMenu(menuList);
		}
		noticeList.clear();
		drawNotice(noticeList);
		kit.renderer.begin();
		kit.renderer.draw(gameList);
		kit.renderer.draw(menuList);
		kit.renderer.draw(noticeList);
		buildKitCommandBuffer();
		submitFrame();
	}

#if defined(PS5_HOST_REFERENCE)
	// Tests on the PC (the host reference build): CDI_TEST_SCRIPT="<frame>:<action> ...", actions play<N> (game N of
	// the list), menu, down, right, cross, circle, list. A test run ignores the pad, so this stands in for it.
	long testFrame{ 0 };
	bool testInputSet{ false };
	hui::InputFrame testInput{};
	void testScript()
	{
		const char *spec = getenv("CDI_TEST_SCRIPT");
		for (const char *c = spec; c && *c;) {
			long f;
			char act[32];
			int n = 0;
			if (std::sscanf(c, "%ld:%31s%n", &f, act, &n) != 2 || n <= 0) {
				break;
			}
			c += n;
			if (f != testFrame) {
				continue;
			}
			std::cerr << "CDI: test frame " << f << ": " << act << std::endl;
			int g;
			hui::InputFrame in{};
			in.connected = true;
			if (std::sscanf(act, "play%d", &g) == 1 && mode == Mode::list) {
				startGame(g - 1);
			} else if (!std::strcmp(act, "view") && mode == Mode::list) {
				nextView();
			} else if (std::sscanf(act, "pic%d", &g) == 1) {
				pictureMode = static_cast<Picture>(std::clamp(g, 0, static_cast<int>(Picture::count) - 1));
			} else if (!std::strcmp(act, "menu") && mode == Mode::playing && !menuOpen) {
				openMenu();
			} else if (menuOpen) {
				if (!std::strcmp(act, "down")) in.nav = hui::Direction::down;
				if (!std::strcmp(act, "right")) in.nav = hui::Direction::right;
				if (!std::strcmp(act, "cross")) in.pressed = hui::action_bit(hui::Action::confirm);
				if (!std::strcmp(act, "circle")) in.pressed = hui::action_bit(hui::Action::back);
				updateMenu(in);
			} else if (!std::strcmp(act, "list") && mode == Mode::playing) {
				backToList();
			} else if (!std::strcmp(act, "options") && mode == Mode::list) {
				setup.show(cdie_lib_count());
			} else if (setup.open && mode == Mode::list) {
				if (!std::strcmp(act, "down")) in.nav = hui::Direction::down;
				if (!std::strcmp(act, "up")) in.nav = hui::Direction::up;
				if (!std::strcmp(act, "right")) in.nav = hui::Direction::right;
				if (!std::strcmp(act, "left")) in.nav = hui::Direction::left;
				if (!std::strcmp(act, "cross")) in.pressed = hui::action_bit(hui::Action::confirm);
				if (!std::strcmp(act, "circle")) in.pressed = hui::action_bit(hui::Action::back);
				if (!std::strcmp(act, "square")) in.pressed = hui::action_bit(hui::Action::west);
				testInput = in;
				testInputSet = true;
			} else if (mode == Mode::list) { // the wheel's own input
				if (!std::strcmp(act, "right")) in.nav = hui::Direction::right;
				if (!std::strcmp(act, "left")) in.nav = hui::Direction::left;
				if (!std::strcmp(act, "cross")) in.pressed = hui::action_bit(hui::Action::confirm);
				if (!std::strcmp(act, "circle")) in.pressed = hui::action_bit(hui::Action::back);
				if (!std::strcmp(act, "triangle")) in.pressed = hui::action_bit(hui::Action::north);
				if (!std::strcmp(act, "r1")) in.pressed = hui::action_bit(hui::Action::page_next);
				testInput = in;
				testInputSet = true;
			}
		}
		testFrame++;
	}
#endif

	void render() override
	{
		if (!prepared) {
			return;
		}
		prepareFrame();
#if defined(PS5_HOST_REFERENCE)
		testScript();
#endif
		const float dt = std::min(frameTimer, 0.05f);
		if (mode == Mode::playing) {
			renderPlaying(dt);
			if (mode == Mode::playing) {
				return;
			}
			// back to the list: this frame was begun; draw the wheel in it below
		}
		// A test run ignores the pad: the design shows its entrance and rests
		hui::InputFrame input = kit.input();
		if (ps5.frameBudget) {
			input = hui::InputFrame{};
			input.connected = true;
		}
#if defined(PS5_HOST_REFERENCE)
		if (testInputSet) {
			input = testInput;
			testInputSet = false;
		}
#endif
		updateTelemetry();
		if (mode == Mode::list) {
			holdOptions(dt);
			housekeeping(dt);
		} else {
			cdie_lib_update(); // the NAS scan may finish meanwhile; merged when the list shows again
		}
		if (mode == Mode::preparing) {
			updatePreparing(input);
			if (mode == Mode::playing) {
				// the game has started: its first frame comes next time
				frame.reset();
				kit.renderer.begin();
				buildKitCommandBuffer();
				submitFrame();
				return;
			}
			input = hui::InputFrame{}; // the design does not see the pad meanwhile
			input.connected = true;
		} else if (mode == Mode::failed) {
			if (input.is_pressed(hui::Action::confirm) || input.is_pressed(hui::Action::back)) {
				mode = Mode::list;
				cdi::playing() = false;
				feedback.play(hui::audio::Cue::back);
			}
			input = hui::InputFrame{};
			input.connected = true;
		}
		if (setup.open && mode == Mode::list) {
			setup.update(input, feedback, cdie_lib_count());
			input = hui::InputFrame{}; // the wheel waits underneath
			input.connected = true;
		}
		if (setup.rescanAsked) {
			setup.rescanAsked = false;
			cdie_lib_rescan();
		}
		screen->update(input, dt, feedback);
		if (context->settings_changed) {
			context->settings_changed = false;
			choices.music_volume = 0;
			kit.apply(choices);
		}
		kit.play(feedback, screen->info().sounds, choices.haptics);
		feedback.clear();
		if (choices.light_bar) {
			const auto &games = cdi::library().games;
			const int c = cdi::centre();
			kit.light_bar(c >= 0 && c < static_cast<int>(games.size()) ? games[static_cast<size_t>(c)].accent : screen->info().accent);
		}
		kit.tick(dt);

		// backdrop, scene, [the glass copy], overlay, post: the order a design is drawn in
		frame.reset();
		frame.glass_texture = kit.renderer.glass_texture();
		screen->draw(frame);
		kit.renderer.begin();
		kit.renderer.backdrop(frame.backdrop);
		kit.renderer.draw(frame.scene);
		if (frame.glass) {
			kit.renderer.glass();
		}
		kit.renderer.draw(frame.overlay);
		kit.renderer.backdrop(frame.post);
		cardList.clear();
		if (mode == Mode::preparing) {
			drawPreparing(cardList);
		} else if (mode == Mode::failed) {
			drawFailed(cardList);
		}
		kit.renderer.draw(cardList);
		setupList.clear();
		if (setup.open && mode == Mode::list) {
			setup.draw(setupList, kit.fonts);
		}
		kit.renderer.draw(setupList);
		leavingList.clear();
		leaving.draw(leavingList, kit.fonts, ps5ui::active_theme());
		kit.renderer.draw(leavingList);
		buildKitCommandBuffer();
		submitFrame();
	}

	~VulkanExample() override
	{
		// the sound callback reads the engine: stop it first
		audio_stop();
		kit.audio = false;
		gameSound = false;
		cdie_shutdown(); // a running game's NVRAM is written
		if (device) {
			vkDeviceWaitIdle(device);
		}
		screen.reset();
		retireTextures();
		dropExtras();
		flushRetired(true);
		for (int i = 0; i < 2; i++) {
			if (pictureTex[i]) {
				kit.renderer.destroy_texture(pictureTex[i]);
			}
		}
		picture.destroy();
		for (int i = 0; i < 2; i++) {
			if (videoTex[i]) {
				kit.renderer.destroy_texture(videoTex[i]);
			}
			video[i].destroy();
		}
		if (g_logFile) {
			std::fclose(g_logFile);
			g_logFile = nullptr;
		}
	}
};

VULKAN_EXAMPLE_MAIN()
