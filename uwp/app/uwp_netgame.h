#pragma once

// Copyright (c) 2026 Skitzelles - part of RPCS3 for Game Vault (github.com/skitzellez/RPCS3-GameVault), GPL-2.0.
// PS3 games played straight from a PC's shared folder or a NAS (Game Vault's network games), no copy first.

#include <string>

namespace uwp_netgame
{
	// "smb://server/share/folder/game" (a game folder or a .iso) -> a path RPCS3's file functions read over the
	// network ("/vfsv0_virtual_game_vault_nas_dev/..."), or "" with *error set. cred_file: Game Vault's sign-in file
	// (lines "server<TAB>share<TAB>user<TAB>password"), read once, written over and deleted; "" = Guest.
	std::string open(const std::string& url, const std::string& cred_file, std::string* error);

	bool is_url(const std::string& path);
}
