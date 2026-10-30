#!/usr/bin/env python3
import argparse
import os
import queue
import random
import select
import socket
import struct
import sys
import threading
import time

MSG_HELLO = 1
MSG_WELCOME = 2
MSG_STATE = 3
MSG_SET_TEAM = 4
MSG_BIND_UID = 5
MSG_CLAIM = 6
MSG_MAP_REQUEST = 7
MSG_MAP_CHUNK = 8
MSG_ERROR = 9

FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
MAP_CHUNK_SIZE = 64 * 1024
MAX_FRAME_SIZE = 2 * 1024 * 1024

PHASE_IDLE = 0
PHASE_LOBBY = 1
PHASE_RUNNING = 2
PHASE_FINISHED = 3

INT_MAX = 2147483647
MEDAL_RULE_NAMES = ["None", "Bronze", "Silver", "Gold", "Author"]


def log(message):
    stamp = time.strftime("%H:%M:%S")
    print(f"[{stamp}] {message}", flush=True)


def pack_u8(value):
    return struct.pack("<B", value & 0xFF)


def pack_u32(value):
    return struct.pack("<I", value & 0xFFFFFFFF)


def pack_i32(value):
    return struct.pack("<i", int(value))


def pack_u64(value):
    return struct.pack("<Q", value & 0xFFFFFFFFFFFFFFFF)


def pack_str(value):
    data = value.encode("utf-8")
    return pack_u32(len(data)) + data


def pack_bytes(data):
    return pack_u32(len(data)) + data


def read_u16(data, offset):
    if offset + 2 > len(data):
        return None
    return struct.unpack_from("<H", data, offset)[0]


def read_u32(data, offset):
    if offset + 4 > len(data):
        return None
    return struct.unpack_from("<I", data, offset)[0]


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0
        self.ok = True

    def _take(self, count):
        if not self.ok or count < 0 or self.pos + count > len(self.data):
            self.ok = False
            return None
        chunk = self.data[self.pos:self.pos + count]
        self.pos += count
        return chunk

    def u8(self):
        chunk = self._take(1)
        return chunk[0] if chunk else 0

    def u32(self):
        chunk = self._take(4)
        return struct.unpack("<I", chunk)[0] if chunk else 0

    def i32(self):
        chunk = self._take(4)
        return struct.unpack("<i", chunk)[0] if chunk else 0

    def u64(self):
        chunk = self._take(8)
        return struct.unpack("<Q", chunk)[0] if chunk else 0

    def string(self):
        length = self.u32()
        chunk = self._take(length)
        return chunk.decode("utf-8", "replace") if chunk is not None else ""

    def bytes(self):
        length = self.u32()
        chunk = self._take(length)
        return chunk if chunk is not None else b""


class Square:
    def __init__(self):
        self.hash = 0
        self.size = 0
        self.display_name = ""
        self.bronze = -1
        self.silver = -1
        self.gold = -1
        self.author = -1
        self.owner_team = -1
        self.record_ms = INT_MAX
        self.record_player = ""
        self.map_uid = ""


class Player:
    def __init__(self, player_id, name, team_id, logged_in):
        self.id = player_id
        self.name = name
        self.team_id = team_id
        self.logged_in = logged_in


class Client:
    def __init__(self, sock, client_id):
        self.sock = sock
        self.id = client_id
        self.player_id = None
        self.in_buf = bytearray()
        self.out = bytearray()
        self.closed = False


def lzo1x_decompress(src, out_len):
    if out_len <= 0:
        return None
    out = bytearray(out_len)
    n = len(src)
    ip = 0
    op = 0

    def need_ip(count):
        return ip + count <= n

    def need_op(count):
        return op + count <= out_len

    if not need_ip(1):
        return None
    t = src[ip]
    if t > 17:
        ip += 1
        t -= 17
        if t < 4:
            state = "match_next"
        else:
            if not need_op(t) or not need_ip(t):
                return None
            out[op:op + t] = src[ip:ip + t]
            op += t
            ip += t
            state = "first_literal_run"
    else:
        state = "outer"

    match = 0
    while True:
        if state == "outer":
            if not need_ip(3):
                return None
            t = src[ip]
            ip += 1
            if t >= 16:
                state = "match"
                continue
            if t == 0:
                while ip < n and src[ip] == 0:
                    t += 255
                    ip += 1
                if ip >= n:
                    return None
                t += 15 + src[ip]
                ip += 1
            count = t + 3
            if not need_op(count) or not need_ip(count):
                return None
            out[op:op + count] = src[ip:ip + count]
            op += count
            ip += count
            state = "first_literal_run"
            continue

        if state == "first_literal_run":
            if not need_ip(1):
                return None
            t = src[ip]
            ip += 1
            if t >= 16:
                state = "match"
                continue
            match = op - (1 + 0x800) - (t >> 2)
            if not need_ip(1):
                return None
            match -= src[ip] << 2
            ip += 1
            if match < 0 or match >= op:
                return None
            if not need_op(3):
                return None
            out[op] = out[match]
            op += 1
            match += 1
            out[op] = out[match]
            op += 1
            match += 1
            out[op] = out[match]
            op += 1
            state = "match_done"
            continue

        if state == "match":
            if t >= 64:
                match = op - 1 - ((t >> 2) & 7)
                if not need_ip(1):
                    return None
                match -= src[ip] << 3
                ip += 1
                t = (t >> 5) - 1
            elif t >= 32:
                t &= 31
                if t == 0:
                    while ip < n and src[ip] == 0:
                        t += 255
                        ip += 1
                    if ip >= n:
                        return None
                    t += 31 + src[ip]
                    ip += 1
                if not need_ip(2):
                    return None
                match = op - 1 - ((src[ip] >> 2) + (src[ip + 1] << 6))
                ip += 2
            elif t >= 16:
                match = op - ((t & 8) << 11)
                t &= 7
                if t == 0:
                    while ip < n and src[ip] == 0:
                        t += 255
                        ip += 1
                    if ip >= n:
                        return None
                    t += 7 + src[ip]
                    ip += 1
                if not need_ip(2):
                    return None
                match -= (src[ip] >> 2) + (src[ip + 1] << 6)
                ip += 2
                if match == op:
                    return bytes(out[:op]) if op == out_len else None
                match -= 0x4000
            else:
                match = op - 1 - (t >> 2)
                if not need_ip(1):
                    return None
                match -= src[ip] << 2
                ip += 1
                if match < 0 or match >= op:
                    return None
                if not need_op(2):
                    return None
                out[op] = out[match]
                op += 1
                match += 1
                out[op] = out[match]
                op += 1
                state = "match_done"
                continue

            if match < 0 or match >= op:
                return None
            if not need_op(t + 2):
                return None
            for _ in range(t + 2):
                out[op] = out[match]
                op += 1
                match += 1
            state = "match_done"
            continue

        if state == "match_done":
            if not need_ip(2):
                return None
            t = src[ip - 2] & 3
            if t == 0:
                state = "outer"
            else:
                state = "match_next"
            continue

        if state == "match_next":
            if not need_op(t) or not need_ip(t + 3):
                return None
            out[op:op + t] = src[ip:ip + t]
            op += t
            ip += t
            t = src[ip]
            ip += 1
            state = "match"
            continue

        return None


def read_medal_times(path):
    try:
        with open(path, "rb") as handle:
            data = handle.read()
    except OSError:
        return (-1, -1, -1, -1)

    if len(data) < 32 or data[:3] != b"GBX":
        return (-1, -1, -1, -1)

    version = read_u16(data, 3)
    if version is None:
        return (-1, -1, -1, -1)
    offset = 5 + 3
    if version >= 4:
        offset += 1
    if offset + 8 > len(data):
        return (-1, -1, -1, -1)

    offset += 4
    if version >= 6:
        user_data_size = read_u32(data, offset)
        if user_data_size is None:
            return (-1, -1, -1, -1)
        offset += 4
        if offset + user_data_size > len(data):
            return (-1, -1, -1, -1)
        offset += user_data_size

    if offset + 8 > len(data):
        return (-1, -1, -1, -1)
    offset += 4
    external_nodes = read_u32(data, offset)
    if external_nodes is None:
        return (-1, -1, -1, -1)
    offset += 4
    if external_nodes != 0:
        return (-1, -1, -1, -1)
    if offset + 8 > len(data):
        return (-1, -1, -1, -1)

    data_size = read_u32(data, offset)
    compressed_size = read_u32(data, offset + 4)
    offset += 8
    if data_size is None or compressed_size is None:
        return (-1, -1, -1, -1)
    if data_size == 0 or data_size > 64 * 1024 * 1024:
        return (-1, -1, -1, -1)
    if compressed_size == 0 or offset + compressed_size > len(data):
        return (-1, -1, -1, -1)

    body = lzo1x_decompress(data[offset:offset + compressed_size], data_size)
    if body is None:
        return (-1, -1, -1, -1)

    index = 0
    while index + 20 <= len(body):
        chunk_id = struct.unpack_from("<I", body, index)[0]
        if chunk_id not in (0x0305B004, 0x2400C004):
            index += 1
            continue
        bronze, silver, gold, author = struct.unpack_from("<iiii", body, index + 4)
        if author <= 0 or gold < author or silver < gold or bronze < silver:
            index += 1
            continue
        if bronze > 24 * 60 * 60 * 1000:
            index += 1
            continue
        return (bronze, silver, gold, author)

    return (-1, -1, -1, -1)


def hash_file(path):
    value = FNV_OFFSET
    size = 0
    try:
        with open(path, "rb") as handle:
            while True:
                block = handle.read(65536)
                if not block:
                    break
                for byte in block:
                    value ^= byte
                    value = (value * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
                size += len(block)
    except OSError:
        return None
    if size <= 0:
        return None
    return value, size


def is_challenge_file(path):
    name = os.path.basename(path).lower()
    return len(name) > 14 and name.endswith(".challenge.gbx")


def map_display_name(path):
    name = os.path.basename(path)
    if len(name) > 14:
        name = name[:-14]
    return name


def square_label(index, board_size):
    if board_size <= 0:
        return "?"
    row, col = divmod(index, board_size)
    return f"{chr(ord('A') + col)}{row + 1}"


class BingoServer:
    def __init__(self, args):
        self.args = args
        self.board_size = args.board_size
        self.team_count = args.teams
        self.medal_rule = args.medal_rule
        self.match_minutes = args.match_minutes
        self.grace_minutes = args.grace_minutes
        self.allow_no_login = not args.require_login

        self.phase = PHASE_IDLE
        self.teams = []
        self.players = []
        self.board = []
        self.winner_team = -1
        self.is_draw = False

        self.map_paths = []
        self.map_files = {}
        self.available_maps = set()

        self.next_client_id = 1
        self.clients = {}
        self.running = True
        self.commands = queue.Queue()

        self.start_time = 0.0
        self.grace_end = 0.0
        self.match_end = 0.0
        self.match_timer_enabled = False
        self.grace_evaluated = False
        self.auto_started = False
        self.boot_time = time.monotonic()
        self.rng = random.Random(args.seed)

        self.listener = None
        self.sync_teams()

    def sync_teams(self):
        if len(self.teams) != self.team_count:
            self.teams = [{"id": index, "name": f"Team {index + 1}"} for index in range(self.team_count)]
        for player in self.players:
            player.team_id = max(0, min(player.team_id, max(0, self.team_count - 1)))

    def rescan_maps(self):
        folder = self.args.map_folder
        self.map_paths = []
        if not os.path.isdir(folder):
            log(f"Map folder does not exist: {folder}")
            return
        for root, _dirs, files in os.walk(folder):
            for name in files:
                path = os.path.join(root, name)
                if is_challenge_file(path) and os.path.isfile(path):
                    self.map_paths.append(path)
        self.map_paths.sort()
        log(f"Bingo map pool: {len(self.map_paths)} maps")

    def generate_board(self):
        if self.phase == PHASE_RUNNING:
            log("Cannot generate a board while a match is running")
            return False

        self.sync_teams()
        board_count = self.board_size * self.board_size
        if len(self.map_paths) < board_count:
            log(f"Map folder needs at least {board_count} maps")
            return False

        shuffled = list(self.map_paths)
        self.rng.shuffle(shuffled)

        selected = []
        seen = set()
        self.map_files = {}
        self.available_maps = set()

        for path in shuffled:
            if len(selected) >= board_count:
                break
            hashed = hash_file(path)
            if hashed is None:
                continue
            digest, size = hashed
            if digest in seen:
                continue
            seen.add(digest)
            square = Square()
            square.hash = digest
            square.size = size
            square.display_name = map_display_name(path)
            if not self.args.skip_medal_times:
                square.bronze, square.silver, square.gold, square.author = read_medal_times(path)
            selected.append(square)
            self.map_files[digest] = path
            self.available_maps.add(digest)
            log(f"Selected {square.display_name} ({len(selected)}/{board_count})")

        if len(selected) < board_count:
            log("Map folder does not contain enough unique maps")
            return False

        self.board = selected
        self.phase = PHASE_LOBBY
        self.winner_team = -1
        self.is_draw = False
        self.grace_evaluated = False
        self.broadcast_state()
        log(f"Generated {self.board_size}x{self.board_size} board")
        return True

    def start_match(self):
        if self.phase == PHASE_RUNNING:
            log("Match already running")
            return
        self.sync_teams()
        board_count = self.board_size * self.board_size
        if len(self.board) != board_count:
            if not self.generate_board():
                return
        for square in self.board:
            square.owner_team = -1
            square.record_ms = INT_MAX
            square.record_player = ""

        now = time.monotonic()
        self.phase = PHASE_RUNNING
        self.winner_team = -1
        self.is_draw = False
        self.start_time = now
        self.grace_end = now + max(0, self.grace_minutes) * 60
        self.match_end = now + max(0, self.match_minutes) * 60
        self.match_timer_enabled = self.match_minutes > 0
        self.grace_evaluated = self.grace_minutes <= 0
        self.broadcast_state()
        log("Match started")

    def stop_match(self):
        if self.phase != PHASE_RUNNING:
            log("No match is running")
            return
        self.finish_match(-1, True)

    def finish_match(self, winner_team, is_draw):
        if self.phase != PHASE_RUNNING:
            return
        self.phase = PHASE_FINISHED
        self.winner_team = winner_team
        self.is_draw = is_draw
        self.broadcast_state()
        if is_draw:
            log("Match ended in a draw")
        elif 0 <= winner_team < len(self.teams):
            log(f"{self.teams[winner_team]['name']} wins")

    def count_squares(self, team_id):
        return sum(1 for square in self.board if square.owner_team == team_id)

    def count_lines_for_team(self, team_id):
        size = self.board_size
        if size <= 0 or len(self.board) < size * size:
            return 0
        count = 0
        for row in range(size):
            if all(self.board[row * size + col].owner_team == team_id for col in range(size)):
                count += 1
        for col in range(size):
            if all(self.board[row * size + col].owner_team == team_id for row in range(size)):
                count += 1
        if all(self.board[i * size + i].owner_team == team_id for i in range(size)):
            count += 1
        if all(self.board[i * size + (size - 1 - i)].owner_team == team_id for i in range(size)):
            count += 1
        return count

    def find_line_teams(self):
        size = self.board_size
        if size <= 0 or len(self.board) < size * size:
            return []
        teams = set()
        for row in range(size):
            team = self.board[row * size].owner_team
            if team >= 0 and all(self.board[row * size + col].owner_team == team for col in range(size)):
                teams.add(team)
        for col in range(size):
            team = self.board[col].owner_team
            if team >= 0 and all(self.board[row * size + col].owner_team == team for row in range(size)):
                teams.add(team)
        team = self.board[0].owner_team
        if team >= 0 and all(self.board[i * size + i].owner_team == team for i in range(size)):
            teams.add(team)
        team = self.board[size - 1].owner_team
        if team >= 0 and all(self.board[i * size + (size - 1 - i)].owner_team == team for i in range(size)):
            teams.add(team)
        return sorted(teams)

    def evaluate_after_grace(self):
        teams = self.find_line_teams()
        if not teams:
            return
        if len(teams) == 1:
            self.finish_match(teams[0], False)
            return

        best_team = -1
        best_lines = -1
        best_squares = -1
        tie = False
        for team in teams:
            lines = self.count_lines_for_team(team)
            squares = self.count_squares(team)
            if lines > best_lines or (lines == best_lines and squares > best_squares):
                best_team = team
                best_lines = lines
                best_squares = squares
                tie = False
            elif lines == best_lines and squares == best_squares:
                tie = True

        if tie or best_team < 0:
            self.finish_match(-1, True)
        else:
            self.finish_match(best_team, False)

    def process_timers(self):
        if self.phase != PHASE_RUNNING:
            return
        now = time.monotonic()

        if not self.grace_evaluated and now >= self.grace_end:
            self.grace_evaluated = True
            self.evaluate_after_grace()
            if self.phase != PHASE_RUNNING:
                return

        if self.match_timer_enabled and now >= self.match_end:
            best_team = -1
            best_count = -1
            tie = False
            for team in self.teams:
                count = self.count_squares(team["id"])
                if count > best_count:
                    best_count = count
                    best_team = team["id"]
                    tie = False
                elif count == best_count:
                    tie = True
            if tie or best_team < 0:
                self.finish_match(-1, True)
            else:
                self.finish_match(best_team, False)

    def serialize_state(self):
        out = bytearray()
        out += pack_u8(self.phase)
        out += pack_i32(self.board_size)
        out += pack_i32(self.team_count)
        out += pack_i32(self.medal_rule)
        out += pack_i32(self.match_minutes)
        out += pack_i32(self.grace_minutes)
        out += pack_u8(1 if self.allow_no_login else 0)
        out += pack_u32(len(self.teams))
        for team in self.teams:
            out += pack_i32(team["id"])
            out += pack_str(team["name"])
        out += pack_u32(len(self.players))
        for player in self.players:
            out += pack_i32(player.id)
            out += pack_str(player.name)
            out += pack_i32(player.team_id)
            out += pack_u8(1 if player.logged_in else 0)
        out += pack_u32(len(self.board))
        for square in self.board:
            out += pack_u64(square.hash)
            out += pack_u64(square.size)
            out += pack_str(square.display_name)
            out += pack_i32(square.bronze)
            out += pack_i32(square.silver)
            out += pack_i32(square.gold)
            out += pack_i32(square.author)
            out += pack_i32(square.owner_team)
            out += pack_i32(square.record_ms)
            out += pack_str(square.record_player)
            out += pack_str(square.map_uid)
        out += pack_i32(self.winner_team)
        out += pack_u8(1 if self.is_draw else 0)
        return bytes(out)

    def build_state_message(self, message_type, player_id=None):
        payload = pack_u8(message_type)
        if player_id is not None:
            payload += pack_i32(player_id)
        payload += self.serialize_state()
        return payload

    def broadcast_state(self):
        payload = self.build_state_message(MSG_STATE)
        for client in list(self.clients.values()):
            self.send_client(client, payload)

    def send_client(self, client, payload):
        if client.closed:
            return
        client.out += pack_u32(len(payload)) + payload

    def send_error(self, client, message):
        self.send_client(client, pack_u8(MSG_ERROR) + pack_str(message))

    def get_player(self, player_id):
        for player in self.players:
            if player.id == player_id:
                return player
        return None

    def handle_client_message(self, client, payload):
        if not payload:
            return
        message_type = payload[0]
        reader = Reader(payload[1:])
        player = self.get_player(client.player_id) if client.player_id is not None else None

        if message_type == MSG_HELLO:
            name = reader.string()
            team = reader.i32()
            logged_in = reader.u8() != 0
            if not reader.ok:
                return
            if not self.allow_no_login and not logged_in:
                self.send_error(client, "This host requires a Nadeo login")
                log(f"Rejected {name or 'player'} (no Nadeo login)")
                self.drop_client(client)
                return
            if not name:
                name = f"Player {client.id}"
            name = name[:40]
            team = max(0, min(team, max(0, self.team_count - 1)))
            if player is None:
                player = Player(client.id, name, team, logged_in)
                self.players.append(player)
                client.player_id = client.id
            else:
                player.name = name
                player.team_id = team
                player.logged_in = logged_in
            self.send_client(client, self.build_state_message(MSG_WELCOME, player.id))
            self.broadcast_state()
            log(f"{name} joined (team {player.team_id + 1})")
            return

        if player is None:
            return

        if message_type == MSG_SET_TEAM:
            team = reader.i32()
            if not reader.ok:
                return
            player.team_id = max(0, min(team, max(0, self.team_count - 1)))
            self.broadcast_state()
            log(f"{player.name} switched to team {player.team_id + 1}")
            return

        if message_type == MSG_BIND_UID:
            index = reader.i32()
            uid = reader.string()
            if not reader.ok or not uid:
                return
            if 0 <= index < len(self.board) and not self.board[index].map_uid:
                self.board[index].map_uid = uid
                self.broadcast_state()
            return

        if message_type == MSG_CLAIM:
            index = reader.i32()
            time_ms = reader.i32()
            uid = reader.string()
            if not reader.ok:
                return
            self.add_claim(player, index, time_ms, uid)
            return

        if message_type == MSG_MAP_REQUEST:
            digest = reader.u64()
            if not reader.ok:
                return
            self.serve_map(client, digest)
            return

    def add_claim(self, player, index, time_ms, uid):
        if self.phase != PHASE_RUNNING:
            return
        if player.team_id < 0 or player.team_id >= len(self.teams):
            return
        if index < 0 or index >= len(self.board):
            return
        if time_ms <= 0:
            return

        square = self.board[index]
        if not square.map_uid and uid:
            square.map_uid = uid
        elif square.map_uid and uid and square.map_uid != uid:
            return
        elif square.map_uid and not uid:
            return

        if square.owner_team >= 0 and time_ms >= square.record_ms:
            return

        square.owner_team = player.team_id
        square.record_ms = time_ms
        square.record_player = player.name
        self.broadcast_state()
        log(f"{player.name} claimed {square_label(index, self.board_size)} in {time_ms} ms")

        if self.grace_evaluated:
            if self.find_line_teams():
                self.evaluate_after_grace()
                return
        self.process_timers()

    def serve_map(self, client, digest):
        on_board = any(square.hash == digest for square in self.board)
        if not on_board:
            self.send_error(client, "Requested map is not part of the board")
            return
        path = self.map_files.get(digest)
        if not path:
            self.send_error(client, "Host does not have the requested map")
            return
        try:
            with open(path, "rb") as handle:
                data = handle.read()
        except OSError:
            self.send_error(client, "Host could not open the requested map")
            return
        if not data:
            return
        offset = 0
        while offset < len(data):
            chunk = data[offset:offset + MAP_CHUNK_SIZE]
            is_last = offset + len(chunk) >= len(data)
            payload = pack_u8(MSG_MAP_CHUNK) + pack_u64(digest) + pack_i32(offset) + pack_u8(1 if is_last else 0) + pack_bytes(chunk)
            self.send_client(client, payload)
            offset += len(chunk)
        log(f"Served map {digest:016x} to {client.player_id}")

    def handle_client_disconnect(self, client):
        if client.player_id is not None:
            player = self.get_player(client.player_id)
            if player is not None:
                self.players.remove(player)
                log(f"{player.name} left")
                self.broadcast_state()
        client.player_id = None

    def drop_client(self, client):
        if client.closed:
            return
        client.closed = True
        self.handle_client_disconnect(client)
        try:
            client.sock.close()
        except OSError:
            pass
        self.clients.pop(client.sock, None)

    def accept_clients(self):
        while True:
            try:
                sock, address = self.listener.accept()
            except BlockingIOError:
                return
            except OSError:
                return
            sock.setblocking(False)
            try:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            except OSError:
                pass
            client = Client(sock, self.next_client_id)
            self.next_client_id += 1
            self.clients[sock] = client
            log(f"Connection from {address[0]}:{address[1]} (id {client.id})")

    def read_client(self, client):
        while True:
            try:
                data = client.sock.recv(65536)
            except BlockingIOError:
                break
            except OSError:
                self.drop_client(client)
                return
            if not data:
                self.drop_client(client)
                return
            client.in_buf += data

        while len(client.in_buf) >= 4:
            size = struct.unpack_from("<I", client.in_buf, 0)[0]
            if size > MAX_FRAME_SIZE:
                self.drop_client(client)
                return
            if size == 0:
                del client.in_buf[:4]
                continue
            if len(client.in_buf) < size + 4:
                break
            payload = bytes(client.in_buf[4:4 + size])
            del client.in_buf[:4 + size]
            self.handle_client_message(client, payload)
            if client.closed:
                return

    def write_client(self, client):
        while client.out:
            try:
                sent = client.sock.send(client.out)
            except BlockingIOError:
                return
            except OSError:
                self.drop_client(client)
                return
            if sent <= 0:
                return
            del client.out[:sent]

    def handle_command(self, line):
        parts = line.split()
        if not parts:
            return
        command = parts[0].lower()
        if command in ("quit", "exit", "stopserver"):
            self.running = False
        elif command in ("start", "go"):
            self.start_match()
        elif command in ("stop", "end"):
            self.stop_match()
        elif command in ("newboard", "generate", "board"):
            self.generate_board()
        elif command in ("rescan", "scan"):
            self.rescan_maps()
        elif command == "status":
            log(f"Phase {self.phase}, {len(self.players)} players, {len(self.board)} squares")
            for team in self.teams:
                log(f"  {team['name']}: {self.count_squares(team['id'])} squares")
            for player in self.players:
                log(f"  player {player.id}: {player.name} (team {player.team_id + 1})")
        elif command == "players":
            for player in self.players:
                log(f"player {player.id}: {player.name} (team {player.team_id + 1})")
        elif command == "kick" and len(parts) >= 2:
            try:
                target = int(parts[1])
            except ValueError:
                log("Usage: kick <player id>")
                return
            for client in list(self.clients.values()):
                if client.player_id == target:
                    log(f"Kicking player {target}")
                    self.drop_client(client)
                    break
        elif command == "help":
            log("Commands: start, stop, newboard, rescan, status, players, kick <id>, quit")
        else:
            log(f"Unknown command: {command}")

    def run(self):
        self.rescan_maps()
        if not self.generate_board() and self.phase != PHASE_LOBBY:
            log("No board generated; use the newboard command once maps are available")

        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((self.args.host, self.args.port))
        listener.listen(socket.SOMAXCONN)
        listener.setblocking(False)
        self.listener = listener
        log(f"Bingo dedicated server listening on {self.args.host}:{self.args.port}")

        keyboard = threading.Thread(target=self.command_thread, daemon=True)
        keyboard.start()

        try:
            while self.running:
                while not self.commands.empty():
                    self.handle_command(self.commands.get())

                self.process_timers()

                if self.args.auto_start and not self.auto_started and self.phase == PHASE_LOBBY:
                    if time.monotonic() - self.boot_time >= self.args.start_delay:
                        self.auto_started = True
                        self.start_match()

                read_socks = [listener]
                write_socks = []
                client_list = list(self.clients.values())
                for client in client_list:
                    read_socks.append(client.sock)
                    if client.out:
                        write_socks.append(client.sock)

                try:
                    readable, writable, _ = select.select(read_socks, write_socks, [], 0.05)
                except (OSError, ValueError):
                    break

                if listener in readable:
                    self.accept_clients()

                for client in list(self.clients.values()):
                    if client.closed:
                        continue
                    if client.sock in readable:
                        self.read_client(client)
                    if client.closed:
                        continue
                    if client.sock in writable:
                        self.write_client(client)
        finally:
            for client in list(self.clients.values()):
                try:
                    client.sock.close()
                except OSError:
                    pass
            try:
                listener.close()
            except OSError:
                pass
            log("Server stopped")

    def command_thread(self):
        try:
            for line in sys.stdin:
                self.commands.put(line.strip())
        except Exception:
            pass


def parse_medal_rule(value):
    lowered = str(value).strip().lower()
    for index, name in enumerate(MEDAL_RULE_NAMES):
        if lowered == name.lower():
            return index
    try:
        number = int(value)
    except ValueError:
        raise argparse.ArgumentTypeError("medal rule must be None, Bronze, Silver, Gold, Author or 0-4")
    if number < 0 or number > 4:
        raise argparse.ArgumentTypeError("medal rule must be 0-4")
    return number


def build_parser():
    parser = argparse.ArgumentParser(description="Dedicated server for the BingoForever TrackMania Bingo module")
    parser.add_argument("--host", default="0.0.0.0", help="address to bind (default 0.0.0.0)")
    parser.add_argument("--port", type=int, default=42123, help="TCP port (default 42123)")
    parser.add_argument("--map-folder", default=".", help="folder to scan recursively for .Challenge.Gbx maps")
    parser.add_argument("--board-size", type=int, default=5, choices=range(3, 8), metavar="3-7", help="board size (default 5)")
    parser.add_argument("--teams", type=int, default=2, choices=range(2, 9), metavar="2-8", help="number of teams (default 2)")
    parser.add_argument("--medal-rule", type=parse_medal_rule, default=0, help="None, Bronze, Silver, Gold, Author or 0-4 (default None)")
    parser.add_argument("--match-minutes", type=int, default=90, help="match length in minutes, 0 for unlimited (default 90)")
    parser.add_argument("--grace-minutes", type=int, default=5, help="grace period in minutes (default 5)")
    parser.add_argument("--require-login", action="store_true", help="reject players without a Nadeo login")
    parser.add_argument("--seed", type=int, default=None, help="random seed for board generation")
    parser.add_argument("--skip-medal-times", action="store_true", help="do not parse medal times from maps")
    parser.add_argument("--auto-start", action="store_true", help="start the match automatically")
    parser.add_argument("--start-delay", type=float, default=10.0, help="seconds to wait before auto-start (default 10)")
    return parser


def main():
    args = build_parser().parse_args()
    args.port = max(1, min(65535, args.port))
    args.match_minutes = max(0, args.match_minutes)
    args.grace_minutes = max(0, args.grace_minutes)
    server = BingoServer(args)
    try:
        server.run()
    except KeyboardInterrupt:
        server.running = False
        log("Interrupted")


if __name__ == "__main__":
    main()
