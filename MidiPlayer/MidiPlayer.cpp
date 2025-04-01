
#include <iostream>
#include <string>
#include <fstream>
#include <math.h>
#include <vector>
#include <Windows.h>
#include <unordered_map>
#include <thread>
#include <filesystem>
#include <tuple>
#include <map>
#include <interception.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <future>
#include <queue>

#include "MidiFile.h"

using namespace smf;
namespace fs = std::filesystem;
std::atomic<bool> muststop(false);
std::atomic<bool> isplaying(false);

std::mutex mtx;
std::condition_variable cv;
std::unordered_map<int, fs::path> pieces = {};
std::unordered_map<int, std::pair<char, char>> scanCodeMap = {
	{0x02, {'1', '!'}}, {0x03, {'2', '@'}}, {0x04, {'3', '#'}},
	{0x05, {'4', '$'}}, {0x06, {'5', '%'}}, {0x07, {'6', '^'}},
	{0x08, {'7', '&'}}, {0x09, {'8', '*'}}, {0x0A, {'9', '('}},
	{0x0B, {'0', ')'}},
	{0x1E, {'a', 'A'}}, {0x30, {'b', 'B'}}, {0x2E, {'c', 'C'}},
	{0x20, {'d', 'D'}}, {0x12, {'e', 'E'}}, {0x21, {'f', 'F'}},
	{0x22, {'g', 'G'}}, {0x23, {'h', 'H'}}, {0x17, {'i', 'I'}},
	{0x24, {'j', 'J'}}, {0x25, {'k', 'K'}}, {0x26, {'l', 'L'}},
	{0x32, {'m', 'M'}}, {0x31, {'n', 'N'}}, {0x18, {'o', 'O'}},
	{0x19, {'p', 'P'}}, {0x10, {'q', 'Q'}}, {0x13, {'r', 'R'}},
	{0x1F, {'s', 'S'}}, {0x14, {'t', 'T'}}, {0x16, {'u', 'U'}},
	{0x2F, {'v', 'V'}}, {0x11, {'w', 'W'}}, {0x2D, {'x', 'X'}},
	{0x15, {'y', 'Y'}}, {0x2C, {'z', 'Z'}},
	{0x0C, {'-', '_'}}, {0x0D, {'=', '+'}},
	{0x1A, {'[', '{'}}, {0x1B, {']', '}'}},
	{0x33, {',', '<'}}, {0x34, {'.', '>'}},
	{0x35, {'/', '?'}},
	{0x39, {' ', ' '}}, {0x1C, {'\n', '\n'}},
};

std::unordered_map<int, char> notetokey = {
	{36, '1'}, {37, '!'}, {38, '2'}, {39, '@'}, {40, '3'},
	{41, '4'}, {42, '$'}, {43, '5'}, {44, '%'}, {45, '6'},
	{46, '^'}, {47, '7'}, {48, '8'}, {49, '*'}, {50, '9'},
	{51, '('}, {52, '0'}, {53, 'q'}, {54, 'Q'}, {55, 'w'},
	{56, 'W'}, {57, 'e'}, {58, 'E'}, {59, 'r'}, {60, 't'},
	{61, 'T'}, {62, 'y'}, {63, 'Y'}, {64, 'u'}, {65, 'i'},
	{66, 'I'}, {67, 'o'}, {68, 'O'}, {69, 'p'}, {70, 'P'},
	{71, 'a'}, {72, 's'}, {73, 'S'}, {74, 'd'}, {75, 'D'},
	{76, 'f'}, {77, 'g'}, {78, 'G'}, {79, 'h'}, {80, 'H'},
	{81, 'j'}, {82, 'J'}, {83, 'k'}, {84, 'l'}, {85, 'L'},
	{86, 'z'}, {87, 'Z'}, {88, 'x'}, {89, 'c'}, {90, 'C'},
	{91, 'v'}, {92, 'V'}, {93, 'b'}, {94, 'B'}, {95, 'n'},
	{96, 'm'}
};

InterceptionContext context;
InterceptionDevice keyboard;

std::tuple<int, bool> returnscancode(char key) {
	for (const auto& entry : scanCodeMap) {
		if (entry.second.first == key) {
			return std::make_tuple(entry.first, false);
		}
		else if (entry.second.second == key) {
			return std::make_tuple(entry.first, true);
		}
	}
	return std::make_tuple(0x1E, true);
}

void keypress(char key, double start, double release) {
	long long starttick = (long long)(start * 1000);
	long long endtick = (long long)(release * 1000);
	
	std::unique_lock<std::mutex> lock(mtx);
	if (cv.wait_for(lock, std::chrono::milliseconds(starttick), [] { return muststop.load(); })) {
		return;
	}

	bool shouldshift = false;
	std::tuple<int, bool> keycodes = returnscancode(key);
	shouldshift = get<1>(keycodes);

	InterceptionKeyStroke stroke = { 0 };
	InterceptionKeyStroke shift = { 0 };

	InterceptionStroke* keyptr = (InterceptionStroke*)&stroke;
	InterceptionStroke* shiftptr = (InterceptionStroke*)&shift;
	shift.code = 0x2A;
	stroke.code = get<0>(keycodes);
	
	if (shouldshift) {
		shift.state = INTERCEPTION_KEY_DOWN;
		interception_send(context, keyboard, shiftptr, 1);
	}

	stroke.state = INTERCEPTION_KEY_DOWN;
	interception_send(context, keyboard, keyptr, 1);

	if (shouldshift) {
		shift.state = INTERCEPTION_KEY_UP;
		interception_send(context, keyboard, shiftptr, 1);
	}

	if (cv.wait_for(lock, std::chrono::milliseconds(endtick), [] { return muststop.load(); })) {
		stroke.state = INTERCEPTION_KEY_UP;
		interception_send(context, keyboard, keyptr, 1);
		return;
	}

	stroke.state = INTERCEPTION_KEY_UP;
	interception_send(context, keyboard, keyptr, 1);
	return;
}

char getkeytoplay(int pitch) {
	return notetokey[pitch];
}

void playpiece(std::string path) {
	isplaying.store(true);

	std::vector<std::thread> notethreads;
	std::map<int, std::tuple<char, double, double>> notes;
	MidiFile midi;
	midi.read(path);

	if (!midi.status()) {
		std::cerr << "failed" << "\n";
		return;
	}

	midi.doTimeAnalysis();
	midi.linkNotePairs();

	for (int track = 0; track < midi.getTrackCount(); track++) {
		for (int i = 0; i < midi[track].size(); i++) {
			MidiEvent& event = midi[track][i];
			if (event.isNoteOn()) {
				int pitch = event.getP1();
				double duration = event.getDurationInSeconds();
				char keytoplay = getkeytoplay(pitch);
				if (keytoplay != '\0') {
					notes[i] = std::make_tuple(keytoplay, event.seconds, duration);
				}
			}
		}
	}
	
	for (auto const& [key, val] : notes)
	{
		char keytoplay = std::get<0>(val);
		double starttime = std::get<1>(val);
		double endtime = std::get<2>(val);
	
		notethreads.emplace_back(keypress, keytoplay, starttime, endtime);
	}

	for (auto& thread : notethreads) {
		thread.join();
	}

	notethreads.clear();
	notes.clear();

	isplaying.store(false);
	muststop.store(false);
	
	return;
}

void listenkeyevents() {
	while (true) {
		for (int keycode = 0; keycode < 256; ++keycode) {
			if (GetAsyncKeyState(keycode) & 0x8000) {
				if (keycode == 192) {
					if (isplaying.load() == true) {
						muststop.store(true);
					}
				}
			}
		}
		Sleep(100);
	}
}


int main(int argc, char** argv) {
	context = interception_create_context();
	keyboard = INTERCEPTION_KEYBOARD(0);
	if (!keyboard) {
		fprintf(stderr, "no keyboard device found.\n");
		interception_destroy_context(context);
	};

	std::thread key(listenkeyevents);
	int index = 1;
	for (const auto &entry : fs::directory_iterator("midi/")) {
		pieces[index] = entry.path();
		index++;
	}
	while (true) {
		if (isplaying.load() == false) {
			std::cout << " " << '\n';
			std::cout << " " << '\n';
			std::cout << "*********Midi Autoplayer********" << '\n';
			std::cout << "******select piece to play******" << '\n';
			std::cout << " " << "\n";

			for (const auto& kvp : pieces) {
				std::cout << "Piece " << kvp.first << "\n";
				std::cout << "Name: " << kvp.second.filename().string() << "\n";
				std::cout << "Path: " << kvp.second.string() << "\n";
				std::cout << " " << "\n";
			}

			std::string input;
			while (input.empty()) {
				std::cout << " " << '\n';
				std::cout << "******input a valid number******" << '\n';
				std::cin >> input;
			}
			int valindex = 0;
			try {
				valindex = static_cast<int>(std::stod(input));
			}
			catch (const std::exception& e) {
				std::cout << "error: " << e.what() << "\n";
				Sleep(500);
				continue;
			}

			if (pieces.find(valindex) != pieces.end()) {
				std::cout << "now playing: " << pieces[valindex].filename().string() << '\n';
				Sleep(1000);
				playpiece(pieces[valindex].string());
				continue;
			}
			else {
				std::cout << "piece does not exist!" << '\n';
				Sleep(500);
				continue;
			}
		}
	}
	key.join();
	interception_destroy_context(context);
	return 0;
}

