# include <Siv3D.hpp> // Siv3D v0.6.15
# include "Multiplayer_Photon.hpp" //いいやつ
# include "PHOTON_APP_ID.SECRET"

enum class TileMaterial : uint8 { Red, Yellow, Gray, Other };

enum class GameState
{
	Home,
	Lobby,
	SteelClimbings,
};

enum class PlayerRole : int32
{
	UpperDefender = 0,
	LowerDefender = 1,
};

enum class TeamSide : int32
{
	Red = 0,
	Blue = 1,
};

struct AssignedPlayer
{
	int32 localID;
	PlayerRole role;
	TeamSide team;
};

// ★ここから追加★
struct MatchState
{
	bool inGame = false;     // カウントダウン完了後 true
	double startTime = 0.0;  // 全員でそろえる開始時刻(秒)
	double countdown = 3.0;  // 表示用
};

// ここで本物を1個だけ作る
MatchState g_match;
// ★ここまで追加★

GameState g_gameState = GameState::Home;
Array<AssignedPlayer> g_assignedPlayers;
bool g_requestGoLobby = false;
bool g_requestGoGame = false;


// HSV でざっくり分類（必要なら境界を調整）
static TileMaterial classifyMaterial(const Color& c)
{
    const HSV hsv(c);
    // 透明は描かない
    if (c.a <= 10) return TileMaterial::Other;

    // 明度が低く彩度も低い→グレー
    if (hsv.s <= 0.15 || hsv.v <= 0.15) return TileMaterial::Gray;

    // 赤系（-10〜30度、330〜360も赤寄りとみなす）
    const double h = hsv.h;
    if (h <= 30 || h >= 330) return TileMaterial::Red;

    // 黄系（30〜75度）
    if (h > 30 && h <= 75) return TileMaterial::Yellow;

    // それ以外は Other（今は描画も一応する）
    return TileMaterial::Other;
}

// ベイク結果：長方形＋材質（平均色はおまけ）
struct TileRect {
	RectF        r;
	TileMaterial mat;   // ← 自作enumで統一
	Color        avg;
};

// 画像を「材質ごとに」大きな矩形へ集約
static Array<TileRect> bakeMapRectsByMaterial(const Image& img, int pixelScale, uint8 alphaThreshold = 10)
{
    const int W = img.width(), H = img.height();
    Array<bool> used(W * H, false);

    auto solid = [&](int x, int y)->bool {
        return (unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H && img[y][x].a > alphaThreshold;
    };
    auto idx = [&](int x, int y)->int { return y * W + x; };

    Array<TileRect> out;

    for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
    {
        if (!solid(x, y) || used[idx(x,y)]) continue;

		const TileMaterial baseMat = classifyMaterial(img[y][x]);

        // 横に伸ばす（同じ材質のみ）
        int w = 0;
        while ((x + w) < W && solid(x + w, y) && !used[idx(x+w,y)]
               && classifyMaterial(img[y][x+w]) == baseMat) ++w;

        // 縦に伸ばす（各行が同幅・同材質・未使用）
        int h = 1;
        bool ok = true;
        while (ok && (y + h) < H)
        {
            for (int xx = 0; xx < w; ++xx)
            {
                if (!solid(x + xx, y + h) || used[idx(x+xx,y+h)]
                    || classifyMaterial(img[y+h][x+xx]) != baseMat) { ok = false; break; }
            }
            if (ok) ++h;
        }

        // 使用済みにして平均色（軽く）を取る
        uint64 sr=0, sg=0, sb=0, sa=0; int cnt=0;
        for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx)
        {
            used[idx(x+xx, y+yy)] = true;
            const Color c = img[y+yy][x+xx];
            sr += c.r; sg += c.g; sb += c.b; sa += c.a; ++cnt;
        }
        Color avg = cnt ? Color(uint8(sr/cnt), uint8(sg/cnt), uint8(sb/cnt), uint8(sa/cnt)) : Palette::White;

        out << TileRect{
            RectF( (x * pixelScale), (y * pixelScale), (w * pixelScale), (h * pixelScale) ),
            baseMat, avg
        };
    }
    return out;
}


// 画像の不透明ピクセル領域を、できるだけ大きな Rect にまとめる
Array<RectF> bakeMapRects(const Image& img, int pixelScale, uint8 alphaThreshold = 10)
{
	const int W = img.width(), H = img.height();
	Array<bool> used(W * H, false);

	auto solid = [&](int x, int y)->bool {
		if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return false;
		return img[y][x].a > alphaThreshold;
		};
	auto mark = [&](int x0, int y0, int w, int h) {
		for (int y = y0; y < y0 + h; ++y)
			for (int x = x0; x < x0 + w; ++x)
				used[y * W + x] = true;
		};
	auto isUsed = [&](int x, int y)->bool {
		return used[y * W + x];
		};

	Array<RectF> out;
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x)
		{
			if (!solid(x, y) || isUsed(x, y)) continue;

			// 横にどこまで伸ばせるか
			int w = 0;
			while ((x + w) < W && solid(x + w, y) && !isUsed(x + w, y)) ++w;

			// 縦にどこまで同じ幅で伸ばせるか（各行が全部 solid で未使用）
			int h = 1;
			bool ok = true;
			while (ok && (y + h) < H)
			{
				for (int xx = 0; xx < w; ++xx)
				{
					if (!solid(x + xx, y + h) || isUsed(x + xx, y + h)) { ok = false; break; }
				}
				if (ok) ++h;
			}

			// 使用済みにして、ワールド座標の RectF を追加
			mark(x, y, w, h);
			out << RectF((x * pixelScale), (y * pixelScale), (w * pixelScale), (h * pixelScale));
		}
	return out;
}

struct NetRemotePlayer
{
	Vec2 pos{ 0, 0 };
	double lastSeenSec = 0.0;
};

extern GameState g_gameState;
extern Array<AssignedPlayer> g_assignedPlayers;
extern bool g_requestGoLobby;
extern bool g_requestGoGame;


class MyNetwork : public Multiplayer_Photon
{
public:



	static constexpr int32 MaxPlayers = 14;

	using Multiplayer_Photon::Multiplayer_Photon;

	// プレイヤーIDと場所の連携ハッシュ
	HashTable<LocalPlayerID, Vec2> m_remotePositions;
	LocalPlayerID m_myLocalID = -1;
	const HashTable<LocalPlayerID, Vec2>& remotePositions() const
	{
		return m_remotePositions;
	}

	// ★ ネットで飛んできた「弾を撃ったよ」を一旦ためるハッシュ
	struct RemoteShot
	{
		Vec2 start;
		Vec2 dir;
		LocalPlayerID shooter;
	};
	Array<RemoteShot> m_remoteShots;
	Array<Array<int32>> m_damageEvents; // 使うなら

	// ★ 今フレーム分のリモート弾をまとめて取り出す
	Array<RemoteShot> fetchRemoteShots()
	{
		auto out = m_remoteShots;
		m_remoteShots.clear();
		return out;
	}
	struct RemoteRope {
		bool hooked = false;
		Vec2 point = Vec2{ 0, 0 }; // 掴んでるワールド座標
		double restLen = 0.0;
	};
	HashTable<LocalPlayerID, RemoteRope> m_remoteRopes;
	const HashTable<LocalPlayerID, RemoteRope>& remoteRopes() const {
		return m_remoteRopes;
	}

	// いまのルームのメンバーを外から見たいので公開
	const Array<LocalPlayer>& localPlayersView() const
	{
		return m_localPlayers;
	}

	// 自分がホストかどうか
	bool isHost() const
	{
		for (const auto& p : m_localPlayers)
		{
			if (p.localID == m_myLocalID)
			{
				return p.isHost;
			}
		}
		return false;
	}

private:

	Array<LocalPlayer> m_localPlayers;

	void connectReturn([[maybe_unused]] const int32 errorCode, const String& errorString, const String& region, [[maybe_unused]] const String& cluster) override
	{
		if (m_verbose)
		{
			Print << U"MyNetwork::connectReturn() [サーバへの接続を試みた結果を処理する]";
		}

		if (errorCode)
		{
			if (m_verbose)
			{
				Print << U"[サーバへの接続に失敗] " << errorString;
			}

			return;
		}

		if (m_verbose)
		{
			Print << U"[サーバへの接続に成功]";
			Print << U"[region: {}]"_fmt(region);
			Print << U"[ユーザ名: {}]"_fmt(getUserName());
			Print << U"[ユーザ ID: {}]"_fmt(getUserID());
		}

		Scene::SetBackground(ColorF{ 0.4, 0.5, 0.6 });
	}

	void disconnectReturn() override
	{
		if (m_verbose)
		{
			Print << U"MyNetwork::disconnectReturn() [サーバから切断したときに呼ばれる]";
		}

		m_localPlayers.clear();

		Scene::SetBackground(Palette::DefaultBackground);
	}

	void joinRandomRoomReturn([[maybe_unused]] const LocalPlayerID playerID, const int32 errorCode, const String& errorString) override
	{
		if (m_verbose)
		{
			Print << U"MyNetwork::joinRandomRoomReturn() [既存のランダムなルームに参加を試みた結果を処理する]";
		}

		if (errorCode == NoRandomMatchFound)
		{
			const RoomName roomName = (getUserName() + U"'s room-" + ToHex(RandomUint32()));

			if (m_verbose)
			{
				Print << U"[参加可能なランダムなルームが見つからなかった]";
				Print << U"[自分でルーム " << roomName << U" を新規作成する]";
			}

			createRoom(roomName, MaxPlayers);

			return;
		}
		else if (errorCode)
		{
			if (m_verbose)
			{
				Print << U"[既存のランダムなルームへの参加でエラーが発生] " << errorString;
			}

			return;
		}

		if (m_verbose)
		{
			Print << U"[既存のランダムなルームに参加できた]";
		}
	}

	void createRoomReturn([[maybe_unused]] const LocalPlayerID playerID, const int32 errorCode, const String& errorString) override
	{
		if (m_verbose)
		{
			Print << U"MyNetwork::createRoomReturn() [ルームを新規作成した結果を処理する]";
		}

		if (errorCode)
		{
			if (m_verbose)
			{
				Print << U"[ルームの新規作成でエラーが発生] " << errorString;
			}

			return;
		}

		if (m_verbose)
		{
			Print << U"[ルーム " << getCurrentRoomName() << U" の作成に成功]";
		}
	}

	void joinRoomEventAction(const LocalPlayer& newPlayer, [[maybe_unused]] const Array<LocalPlayerID>& playerIDs, const bool isSelf) override
	{
		if (m_verbose)
		{
			Print << U"MyNetwork::joinRoomEventAction() [誰か（自分を含む）が現在のルームに参加したときに呼ばれる]";
		}

		m_localPlayers = getLocalPlayers();

		if (m_verbose)
		{
			Print << U"[{} (ID: {}) がルームに参加した。ローカル ID: {}] {}"_fmt(newPlayer.userName, newPlayer.userID, newPlayer.localID, (isSelf ? U"(自分自身)" : U""));

			Print << U"現在の " << getCurrentRoomName() << U" のルームメンバー";

			for (const auto& player : m_localPlayers)
			{
				Print << U"- [{}] {} (id: {}) {}"_fmt(player.localID, player.userName, player.userID, player.isHost ? U"(host)" : U"");
			}
		}

		if (isSelf)
		{
			m_myLocalID = newPlayer.localID; // ★ 自分のIDを保持

			// ★ ここでロビーに行く合図を出す
			g_requestGoLobby = true;
		}
	}


	void leaveRoomEventAction(const LocalPlayerID playerID, [[maybe_unused]] const bool isInactive) override
	{
		if (m_verbose)
		{
			Print << U"MyNetwork::leaveRoomEventAction() [誰かがルームから退出したら呼ばれる]";
		}

		m_localPlayers = getLocalPlayers();

		if (m_verbose)
		{
			for (const auto& player : m_localPlayers)
			{
				if (player.localID == playerID)
				{
					Print << U"[{} (ID: {}, ローカル ID: {}) がルームから退出した]"_fmt(player.userName, player.userID, player.localID);
				}
			}

			Print << U"現在の " << getCurrentRoomName() << U" のルームメンバー";

			for (const auto& player : m_localPlayers)
			{
				Print << U"- [{}] {} (ID: {}) {}"_fmt(player.localID, player.userName, player.userID, player.isHost ? U"(host)" : U"");
			}
		}
	}

	void leaveRoomReturn(int32 errorCode, const String& errorString) override
	{
		if (m_verbose)
		{
			Print << U"MyNetwork::leaveRoomReturn() [ルームから退出したときに呼ばれる]";
		}

		m_localPlayers.clear();

		if (errorCode)
		{
			if (m_verbose)
			{
				Print << U"[ルームからの退出でエラーが発生] " << errorString;
			}

			return;
		}
	}
	void customEventAction(const LocalPlayerID playerID, const uint8 eventCode, const int32 data) override
	{
		Print << U"<<< [" << playerID << U"] からの eventCode: " << eventCode << U", data: int32(" << data << U") を受信";
	}

	// String を受信したときに呼ばれる関数をオーバーライドしてカスタマイズする
	void customEventAction(const LocalPlayerID playerID, const uint8 eventCode, const String& data) override
	{
		Print << U"<<< [" << playerID << U"] からの eventCode: " << eventCode << U", data: String(" << data << U") を受信";
	}

	// Point を受信したときに呼ばれる関数をオーバーライドしてカスタマイズする
	void customEventAction(const LocalPlayerID playerID, const uint8 eventCode, const Point& data) override
	{
		Print << U"<<< [" << playerID << U"] からの eventCode: " << eventCode << U", data: Point" << data << U" を受信";
	}

	// Array<int32> を受信したときに呼ばれる関数をオーバーライドしてカスタマイズする
	// 簡単に言ったらここが位置座標を受け取る関数みたいなものですね　そう考えたらいいです👍
	// Array<int32> を受信したとき（＝今回の座標送信用）
	void customEventAction(const LocalPlayerID playerID, const uint8 eventCode, const Array<int32>& data) override
	{
		// 0: 位置
		if (eventCode == 0 && data.size() >= 2)
		{
			if (playerID != m_myLocalID)
			{
				m_remotePositions[playerID] = Vec2{ (double)data[0], (double)data[1] };
			}
		}
		else if (eventCode == 1 && data.size() >= 5)
		{
			// data: [startX, startY, dirX*1000, dirY*1000, shooterID]
			RemoteShot rs;
			rs.start = Vec2{ (double)data[0], (double)data[1] };
			rs.dir = Vec2{ data[2] / 1000.0, data[3] / 1000.0 };
			rs.shooter = playerID;              // 送ってきた人でOK（data[4]でもいい）

			m_remoteShots << rs;
		}
		// ★ 3: ロープ
		else if (eventCode == 3)
		{
			// パターンを2通りにします
			//   data.size()==1 → 離した
			//   data.size()>=4 → 掴んだ（x,y,restLen*100, 1=hooked）
			auto& rope = m_remoteRopes[playerID];

			if (data.size() == 1)
			{
				// 離した
				rope.hooked = false;
			}
			else if (data.size() >= 4)
			{
				const int32 ix = data[0];
				const int32 iy = data[1];
				const int32 rest100 = data[2];
				const int32 hookedFlag = data[3];

				rope.hooked = (hookedFlag != 0);
				rope.point = Vec2{ (double)ix, (double)iy };
				rope.restLen = rest100 / 100.0;
			}
		}
		// 10: ゲーム開始＋役割配布
		else if (eventCode == 10)
		{
			// ★ ゲーム開始パケットを受信
			// data[0] = startAtMillis
			// data[1] = playerCount
			if (data.size() < 2)
			{
				return;
			}

			const int32 startAtMillis = data[0];
			const int32 playerCount = data[1];

			g_assignedPlayers.clear();
			g_assignedPlayers.reserve(playerCount);

			int index = 2;
			for (int i = 0; i < playerCount; ++i)
			{
				if (index + 2 >= data.size())
				{
					break;
				}
				AssignedPlayer ap;
				ap.localID = data[index + 0];
				ap.role = static_cast<PlayerRole>(data[index + 1]);
				ap.team = static_cast<TeamSide>(data[index + 2]);
				g_assignedPlayers << ap;

				index += 3;
			}

			// グローバルのマッチ情報に反映
			g_match.inGame = false; // まだカウントダウン中
			g_match.startTime = startAtMillis / 1000.0; // 秒に戻す
			g_match.countdown = 3.0; // 見た目カウントダウン

			// シーンをゲームに進める合図
			g_requestGoGame = true;
		}

		if (m_verbose)
		{
			Print << U"<<< [" << playerID << U"] event " << eventCode << U" data " << data;
		}
	}

	// Array<String> を受信したときに呼ばれる関数をオーバーライドしてカスタマイズする
	void customEventAction(const LocalPlayerID playerID, const uint8 eventCode, const Array<String>& data) override
	{
		Print << U"<<< [" << playerID << U"] からの eventCode: " << eventCode << U", data: Array<String>" << data << U" を受信";
	}
};



void Main()
{

	//基本設定

	const std::string secretAppID{	SIV3D_OBFUSCATE(PHOTON_APP_ID) };
	MyNetwork network{ secretAppID, U"1.0", Verbose::Yes };

	// 画面
	constexpr int32 WindowX = 1200, WindowY = 800;
	Window::Resize(WindowX, WindowY);


	// === プレイヤー ===
	Vec2 pos = { 400, 300 };
	Vec2 vel = { 0, 0 };
	constexpr double PlayerSize = 40.0;           // AABB サイズ
	constexpr double Half = PlayerSize / 2.0;
	bool onGround = false;

	// === カメラ ===
	Vec2 cam = pos;

	//変数

	//Font
	const Font font{ FontMethod::MSDF, 48 };
	// HUD用フォント（小さめ／大きめ）
	const Font hud{ FontMethod::MSDF, 24 };
	const Font big{ FontMethod::MSDF, 40 };

	// 体力
	int hpMax = 100;
	int hp = hpMax;

	//効果音
	const Audio SG{ U"SG.mp3" };
	const Audio AR{ U"AR.mp3" };

	// === 物理 ===
	constexpr double Accel = 1.0;
	constexpr double Friction = 0.9;
	constexpr double MaxSpeedX = 7.0;
	constexpr double Gravity = 0.6;
	constexpr double JumpPower = 17.0;

	// === フック（ターザン） ===
	bool isHooked = false;
	Vec2 hookPoint = { 0, 0 };
	double ropeRestLen = 150.0;
	constexpr double Kspring = 0.15;
	constexpr double Cdamp = 0.08;
	constexpr double HookRange = 400.0;
	int hookCool = 0;

	// === マップ画像（当たり判定 & 描画） ===
	// === マップ画像（当たり判定 & 描画） ===
	constexpr int PixelScale = 3;
	const Vec2 mapOrigin = { 0, 0 };
	
	// === マップ画像（当たり判定 & 描画） ===
	// ===== マップの素材を全部先に用意 =====
	const Image lobbyMap{ U"The_Lobby.png" };
	const Image steelMap{ U"STEEL_CLIMBINGS.png" };

	const Texture lobbyTexture{ lobbyMap.scaled(PixelScale) };
	const Texture steelTexture{ steelMap.scaled(PixelScale) };

	Array<TileRect> lobbyTiles = bakeMapRectsByMaterial(lobbyMap, PixelScale);
	Array<TileRect> steelTiles = bakeMapRectsByMaterial(steelMap, PixelScale);

	Array<RectF> lobbyRects = bakeMapRects(lobbyMap, PixelScale);
	Array<RectF> steelRects = bakeMapRects(steelMap, PixelScale);


	// ===== 今使ってるマップを保持する変数 =====
	Image map = lobbyMap;
	Texture mapTexture = lobbyTexture;
	Array<TileRect> mapTiles = lobbyTiles;
	Array<RectF> mapRects = lobbyRects;

	// 今使ってるステージを指すポインタ
	const Image* currentMap = &lobbyMap;
	const Texture* currentTex = &lobbyTexture;
	Array<TileRect>* currentTiles = &lobbyTiles;
	Array<RectF>* currentRects = &lobbyRects;

	//変数終了;

	auto toScreenRect = [&](RectF r) {
		r.x = r.x - cam.x + WindowX / 2.0;
		r.y = r.y - cam.y + WindowY / 2.0;
		return r;
		};

	//銃設定

	// === 武器 ===
	enum class Weapon { Rifle, Shotgun, SMG };
	Weapon weapon = Weapon::Rifle;

	// 射撃パラメータ
	struct GunParams {
		double cooldown;      // 発射間隔[秒] 例: 0.12
		int    pellets;       // 1発あたり弾数（ライフル=1, SG=8など）
		double spreadDeg;     // 拡散角度（1ペレットあたり）
		double range;         // 有効射程（ray長）
	};
	GunParams rifle{ 0.12, 1,  0.6, 700.0 };
	GunParams shotgun{ 0.45, 8,  6.0, 450.0 };
	GunParams smg{ 0.06, 1, 1.5, 600.0 };


	// --- 武器リスト
	Array<String> weaponName{ U"SM-68 Rifle", U"ANV SG", U"SMG"};
	Array<Weapon> weaponList{ Weapon::Rifle, Weapon::Shotgun, Weapon::SMG };
	Array<int> damage{ 24, 13, 9 };
	Array<int> weaponMagSize{ 30, 8, 25 };
	Array<bool> weaponIsAuto{ true, false, true};
	Array<int> weaponReserveInit{ 120, 32, 75};
	Array<int> mag{ 30, 8, 25};

	// 追加: 実弾ストレージ（各武器ごと）
	int weaponIndex = 0;
	Array<int> gMag(weaponList.size());
	Array<int> gReserve(weaponList.size());
	for (size_t i = 0; i < weaponList.size(); ++i) {
		gMag[i] = weaponMagSize[i];
		gReserve[i] = weaponReserveInit[i];
	}
	auto currentWeapon = [&]() -> Weapon { return weaponList[weaponIndex]; };
	auto paramsOf = [&](Weapon w) -> const GunParams& {
		return (w == Weapon::Rifle) ? rifle
			: (w == Weapon::Shotgun) ? shotgun
			: smg;
		};

	// …同様に使用箇所も
	const GunParams& g = (weapon == Weapon::Rifle) ? rifle
		: (weapon == Weapon::Shotgun) ? shotgun
		: smg;

	double shootCooldown = 0.0;   // 残りクールダウン
	int ammoInMag = weaponMagSize[weaponIndex];// マガジン弾数
	int    magSize = 25;
	int    reserve = 100;        // 残弾（とりあえず無限気味）
	bool   muzzleOn = false;      // マズルフラッシュ表示フラグ
	double muzzleTimer = 0.0;     // マズル表示時間

	// 命中デカール（火花/弾痕）管理
	struct Impact { Vec2 pos; double t; };
	Array<Impact> impacts;

	// ★ 追加：弾道トレーサー（短い寿命のライン）
	struct Tracer { Vec2 a, b; double t; };
	Array<Tracer> tracers;


	//銃設定終了

	//照準
	// ===== エイム可視化 & スプレッド =====
	double spreadBloomDeg = 0.0;         // 発砲や移動で増え、徐々に減衰
	constexpr double BloomDecayPerSec = 25.0;     // 1秒にこれだけ縮む（度）
	constexpr double MoveBloomFactor = 0.15;     // 移動速度→ブルーム寄与係数

	// 武器別のブルーム増加（1発あたり）
	constexpr double BloomAddRifle = 4.0;
	constexpr double BloomAddShotgun = 12.0;
	constexpr double BloomMax = 30.0;      // ブルームの上限（度）

	// エイムプレビュー
	constexpr int AimPreviewPellets = 6;          // 予測点の数（SG時）
	constexpr double AimPreviewAlpha = 0.35;      // 透明度


	auto withSpread = [](const Vec2& dir, double deg)
		{
			double rad = Math::ToRadians(deg);
			return Vec2(
				dir.x * Cos(rad) - dir.y * Sin(rad),
				dir.x * Sin(rad) + dir.y * Cos(rad)
			).normalized();
		};

	// === 敵 ===
	struct Enemy {
		Vec2   pos;          // 中心座標（ワールド）
		Vec2   vel{ 0, 0 };
		double w{ 34 }, h{ 46 }; // 当たり判定サイズ
		int    hpMax{ 60 }, hp{ 60 };

		bool alive() const { return hp > 0; }
		RectF aabb() const { return RectF(Arg::center(pos), w, h); }
	};

	Array<Enemy> enemies = {
	};


	// --- ヘルパ ---
	auto toScreen = [&](const Vec2& wp) {
		return Vec2(wp.x - cam.x + WindowX / 2.0, wp.y - cam.y + WindowY / 2.0);
		};

	// 角度(deg)で回転した正規化ベクトルを返す
	

	// 画像マップに対するレイ（最初に当たる点）。既存 raycastHookMap と同様。
	


	// 画像マップの指定ピクセルが「壁」か？（黒っぽい & ほぼ不透明を壁扱い）
	// --- 壁判定（＝不透明ピクセルなら壁 とする） ---
	auto isSolidPixel = [&](int ix, int iy)->bool {
		if (ix < 0 || iy < 0 || ix >= (int)map.width() || iy >= (int)map.height())
			return false;
		const Color c = map[iy][ix];
		return (c.a > 10);
		};




	// ワールド -> ピクセル
	auto worldToPixel = [&](const Vec2& wp)->Point {
		const Vec2 rel = wp - mapOrigin;
		return Point((int)Math::Floor(rel.x / PixelScale),
					 (int)Math::Floor(rel.y / PixelScale));
		};

	// 半径 pad ピクセルの近傍まで厚みを見て “壁” を返す（細線のすり抜け防止）
	auto isSolidAt = [&](const Vec2& wp, int pad = 1)->bool {
		const Point p = worldToPixel(wp);
		for (int dy = -pad; dy <= pad; ++dy) {
			for (int dx = -pad; dx <= pad; ++dx) {
				if (isSolidPixel(p.x + dx, p.y + dy)) return true;
			}
		}
		return false;
		};



	// マップへのレイキャスト（pad 付き）
	auto raycastHookMap = [&](const Vec2& origin, const Vec2& dir, double maxDist)->Optional<Vec2> {
		const double step = PixelScale; // スケールと同等刻みで抜けにくく
		for (double d = 0; d <= maxDist; d += step) {
			const Vec2 cur = origin + dir * d;
			if (isSolidAt(cur, 1)) return cur; // 近傍1でチェック
		}
		return none;
		};

	auto raycastBullet = [&](const Vec2& origin, const Vec2& dir, double maxDist)->Optional<Vec2> {
		const double step = PixelScale; // 抜け防止
		for (double d = 0; d <= maxDist; d += step) {
			const Vec2 p = origin + dir * d;
			if (isSolidAt(p, 1)) return p;
		}
		return none;
		};

	// レイ（origin→dir*maxDist）で「最初に当たる敵」を返す
	struct EnemyHit { size_t index; Vec2 point; double dist; };

	auto raycastEnemy = [&](const Vec2& origin, const Vec2& dir, double maxDist) -> Optional<EnemyHit>
		{
			const double step = PixelScale; // マップと同じ刻みでOK
			Optional<EnemyHit> best;
			for (double d = 0; d <= maxDist; d += step) {
				const Vec2 p = origin + dir * d;
				for (size_t i = 0; i < enemies.size(); ++i) {
					if (!enemies[i].alive()) continue;
					if (enemies[i].aabb().contains(p)) { // 点がAABBに入った瞬間
						best = EnemyHit{ i, p, d };
						return best; // 最初に見つかった＝最短
					}
				}
			}
			return none;
		};


	auto drawHUD = [&]() {
		const double pad = 16;

		// ====== Health（左上）======
		{
			const double w = 280, h = 42;
			RoundRect bg(RectF(pad, 800 - pad - h, w, h), 12);
			bg.draw(ColorF(0.05, 0.06, 0.08, 0.75))
				.drawFrame(2, 0, ColorF(1, 1, 1, 0.07));

			const double rate = Clamp((double)hp / hpMax, 0.0, 1.0);
			const RectF fill(bg.rect.pos.movedBy(4, 4), (w - 8) * rate, h - 8);
			// 置き換え後
			RoundRect(fill, 10)
				.draw(ColorF(0.98, 0.40, 0.28, 0.95)); // 赤〜オレンジの中間色


			// ラベル＆数値
			hud(U"HP").draw(20, Vec2(bg.rect.x + 10, bg.rect.y + 8), ColorF(1));
			big(U"{:>3}"_fmt(hp)).draw(20, Vec2(bg.rect.x + w - 96, bg.rect.y + 2), ColorF(1));
		}

		// ====== Ammo / Weapon（右下）======
		{
			const GunParams& g = paramsOf(currentWeapon());

			const double panelW = 320, panelH = 120;
			const Vec2 p(WindowX - panelW - pad, WindowY - panelH - pad);

			RoundRect card(RectF(p, panelW, panelH), 18);
			card.draw(ColorF(0.05, 0.06, 0.08, 0.78))
				.drawFrame(2, 0, ColorF(1, 1, 1, 0.07));

			// ヘッダー（武器名＋モード）
			const bool isAuto = weaponIsAuto[weaponIndex];
			const String mode = (isAuto ? U"Auto" : U"Semi");
			hud(U"{}  ·  {}"_fmt(weaponName[weaponIndex], mode))
				.draw(22, p.movedBy(16, 8), ColorF(0.95));

			// 大きな弾数表示
			big(U"{}/{}"_fmt(ammoInMag, reserve))
				.draw(26, p.movedBy(16, 44), ColorF(1));

			// マガジンゲージ
			const double rateMag = Clamp((double)ammoInMag / Max(1, magSize), 0.0, 1.0);
			const RectF magBG(p.movedBy(16, 90), panelW - 32, 12);
			RoundRect(magBG, 6).draw(ColorF(1, 1, 1, 0.08));
			// 置き換え後
			RoundRect(RectF(magBG.pos, (panelW - 32)* rateMag, 12), 6)
				.draw(ColorF(0.78, 0.82, 1.00, 0.90)); // 淡いブルー


			// 連射クールダウンの薄い上書き（発砲直後だけ見える）
			const double cd = (shootCooldown > 0.0 ? Saturate(shootCooldown / Max(0.001, g.cooldown)) : 0.0);
			if (cd > 0.0) {
				const RectF cdR = RectF(magBG.pos, (panelW - 32) * cd, 12);
				RoundRect(cdR, 6).draw(ColorF(0.2, 0.25, 0.45, 0.55));
			}

			// リロードプロンプト
			if (ammoInMag == 0) {
				hud(U"Press [R] to Reload")
					.draw(18, p.movedBy(panelW - 200, 46), ColorF(1.0, 0.7, 0.7, 0.9));
			}
		}
		};


	//ここからループ処理！！！見やすくしといてやるから感謝しろ！！！！



	while (System::Update())
	{

		network.update();

		const double dt = Scene::DeltaTime();
		// network.update(); のあとあたりで
		static GameState prevState = g_gameState;

		// ここでロビー / ゲームに進む合図を反映
		if (g_requestGoLobby) { g_gameState = GameState::Lobby; g_requestGoLobby = false; }
		if (g_requestGoGame) { g_gameState = GameState::SteelClimbings; g_requestGoGame = false; }

		if (prevState != g_gameState)
		{
			if (g_gameState == GameState::Lobby)
			{
				map = lobbyMap;
				mapTexture = lobbyTexture;
				mapTiles = lobbyTiles;
				mapRects = lobbyRects;
			}
			else if (g_gameState == GameState::SteelClimbings)
			{
				map = steelMap;
				mapTexture = steelTexture;
				mapTiles = steelTiles;
				mapRects = steelRects;
			}
			prevState = g_gameState;
		}

		if (g_gameState == GameState::Home)
		{
			// 背景
			Scene::SetBackground(ColorF(0.02, 0.02, 0.05));

			big(U"Siv3D Online (Home)").drawAt(Scene::Center().movedBy(0, -140));

			if (SimpleGUI::Button(U"Connect", Vec2{ 500, 300 }, 200, (not network.isActive())))
			{
				const String userName = U"Player";
				network.connect(userName);
			}

			if (g_gameState == GameState::Lobby)
			{
				// 背景をロビー用に
				Scene::SetBackground(ColorF(0.10, 0.12, 0.16));

				const auto& members = network.localPlayersView();

				// メンバー表示
				int y = 80;
				big(U"Lobby - {} players"_fmt(members.size())).draw(40, 20, Palette::White);
				for (const auto& m : members)
				{
					String tag = m.userName + U" (id:" + Format(m.localID) + U")";
					if (m.isHost) tag += U" [HOST]";
					big(tag).draw(60, y, Palette::White);
					y += 28;
				}

				// ホストだけ「ゲーム開始」
				if (network.isHost())
				{
					if (SimpleGUI::Button(U"ゲーム開始", Vec2{ 600, 40 }, 180))
					{
						const auto& members = network.localPlayersView();

						// 今から1秒後を開始時刻にする（通信ぶんの猶予）
						const double startAt = Time::GetSec() + 1.0;

						Array<int32> payload;
						payload << (int32)(startAt * 1000);   // double は送れないのでミリ秒にしてint化
						payload << (int32)members.size();     // 人数

						for (size_t i = 0; i < members.size(); ++i)
						{
							const auto& m = members[i];

							const PlayerRole role = (i % 2 == 0) ? PlayerRole::UpperDefender : PlayerRole::LowerDefender;
							const TeamSide   team = (i % 2 == 0) ? TeamSide::Red : TeamSide::Blue;

							payload << (int32)m.localID;
							payload << (int32)role;
							payload << (int32)team;
						}

						// 10 = START_GAME
						network.sendEvent(10, payload);
					}
				}


				// ロビーでもプレイヤーの描画とか許可したかったらこのまま下の「ゲーム本体処理」に落とす
				// 「ロビーは単に待つだけ」でいいならここで continue; でもOK
				// 今回は "ロビー画像を使ってもいい" と思うので落とさずにおきます
			}

			// 状態に応じて使うステージを決める
			if (g_gameState == GameState::Lobby)
			{
				currentMap = &lobbyMap;
				currentTex = &lobbyTexture;
				currentTiles = &lobbyTiles;
				currentRects = &lobbyRects;
			}
			else if (g_gameState == GameState::SteelClimbings)
			{
				currentMap = &steelMap;
				currentTex = &steelTexture;
				currentTiles = &steelTiles;
				currentRects = &steelRects;
				// === SteelClimbings 中の開始カウントダウン ===
				if (g_gameState == GameState::SteelClimbings)
				{
					const double now = Time::GetSec();
					const double remain = g_match.startTime - now; // 0 になったら開始

					if (remain <= 0.0)
					{
						g_match.inGame = true;
					}
					else
					{
						g_match.inGame = false;
						g_match.countdown = remain; // 表示用に今の残りを入れておく
					}
				}
				else
				{
					// ゲーム中じゃないときは一旦リセット気味にしておく
					g_match.inGame = false;
				}

			}


			if (network.isActive())
			{
				if (SimpleGUI::Button(U"Join Random Room", Vec2{ 500, 350 }, 200))
				{
					network.joinRandomRoom(MyNetwork::MaxPlayers);
				}

				if (SimpleGUI::Button(U"Create Room", Vec2{ 500, 400 }, 200))
				{
					// 直接作るパターン
					const RoomName roomName = U"room-" + ToHex(RandomUint32());
					network.createRoom(roomName, MyNetwork::MaxPlayers);
					// createRoomReturn / joinRoomEventAction でロビーに行く
				}
			}

			// ホームならこのフレームはここで終わり
			continue;
		}


		// ★ ここで他クライアントが撃った弾を回収して再生する
		for (const auto& shot : network.fetchRemoteShots())
		{
			// 可視化（相手の弾も見えるように）
			const Vec2 start = shot.start;
			const Vec2 end = shot.start + shot.dir * 700.0; // 射程はあなたのg.rangeに合わせて

			tracers << Tracer{ start, end, 0.06 };
			impacts << Impact{ end, 0.20 };

			// ★ 自分に当たってるかざっくり見る（AABB vs line）→当てられてたらHPを減らす
			const RectF myRect(Arg::center(pos), PlayerSize, PlayerSize);

			// 線分が自分のAABBを通過してるかをちょっと雑にチェック
			// （本当にちゃんとやるならRectFとLineの交差を書きますが、ここでは簡易でOK）
			const Vec2 toMe = pos - start;
			const double proj = toMe.dot(shot.dir); // 射線上のどこか
			if (0.0 <= proj && proj <= 700.0)
			{
				const Vec2 closest = start + shot.dir * proj;
				if (myRect.contains(closest))
				{
					hp = Max(0, hp - 15); // ダメージ量は適当に
				}
			}
		}

		if (g_gameState == GameState::SteelClimbings && !g_match.inGame)
		{
			// 入力は受け取らないで、位置更新もしない
		}
		else
		{
			// ブルームを減衰
			spreadBloomDeg = Max(0.0, spreadBloomDeg - BloomDecayPerSec * dt);

			// 移動によるブルーム（走り撃ちで広がる感じ）
			double moveSpeed = Abs(vel.x) + Abs(vel.y);
			spreadBloomDeg = Min(BloomMax, spreadBloomDeg + moveSpeed * MoveBloomFactor * dt);


			// 入力・重力
			const int hdir = (KeyD.pressed() - KeyA.pressed());
			vel.x += hdir * Accel;
			vel.x *= Friction;
			vel.x = Clamp(vel.x, -MaxSpeedX, MaxSpeedX);
			vel.y += Gravity;

			// マウス方向（v0.6.15 には hasLength がないので length で判定）
			const Vec2 mouseWorld = Vec2(Cursor::Pos()) + (cam - Vec2(WindowX / 2.0, WindowY / 2.0));
			const Vec2 diff = (mouseWorld - pos);
			const Vec2 dirToMouse = (diff.length() > 0.0001) ? diff.normalized() : Vec2{ 1, 0 };

			// --- 弾道トレーサー（手前に細い線をサッと描いて消える） ---
			for (const auto& tr : tracers) {
				double a = Saturate(tr.t * 16.0); // フェード（0.06s → 素早く消える）
				Line(toScreen(tr.a), toScreen(tr.b)).draw(2, ColorF(1.0, 0.95, 0.7, a));
			}

			// --- 火花＆弾痕（命中点にちょい残る） ---
			for (const auto& im : impacts) {
				double a = Saturate(im.t * 4.0); // 0.25s → ゆっくり消える
				Circle(toScreen(im.pos), 6).draw(ColorF(1.0, 0.8, 0.2, a));   // 火花
				Circle(toScreen(im.pos), 2).draw(ColorF(0.1, 0.1, 0.1, a));   // 弾痕
			}

			// --- マズルフラッシュ（プレイヤー位置を銃口代わり） ---
			if (muzzleOn) {
				Circle(toScreen(pos), 10).draw(ColorF(1.0, 1.0, 0.6, 0.85));
			}


			// フック射出（左クリック：マップにのみヒット）
			if (MouseR.down() && !isHooked && hookCool <= 0) {
				hookCool = 20;
				if (auto hit = raycastHookMap(pos, dirToMouse, HookRange)) {
					isHooked = true;
					hookPoint = *hit;
					ropeRestLen = pos.distanceFrom(*hit) * 0.9;

					// ★ ネットにも送る: [x, y, restLen*100, 1]
					Array<int32> ropeData = {
						(int32)hookPoint.x,
						(int32)hookPoint.y,
						(int32)(ropeRestLen * 100),
						1
					};
					network.sendEvent(3, ropeData);
				}
			}

			// 離す（左ボタンを離したら）
			if (MouseR.up()) {
				isHooked = false;

				// ★ 離した: [0]
				network.sendEvent(3, Array<int32>{ 0 });
			}

			// フック中の力（バネ + 減衰 + 接線こぎ）
			if (isHooked) {
				// --- W 押した瞬間に「フックジャンプ」して離す ---
				bool didHookJump = false;
				if (KeyW.down()) {
					Vec2 r = pos - hookPoint;
					const double len = r.length();
					if (len > 1e-3) {
						const Vec2 dirR = r / len;                 // アンカー→プレイヤー（外向き）
						const Vec2 tangent = Vec2(-dirR.y, dirR.x);// 接線方向

						// いまの速度の接線向きに合わせる（左回り/右回りで向きを自動決定）
						const double sgn = (vel.dot(tangent) >= 0.0) ? 1.0 : -1.0;

						// 好みで調整
						constexpr double TangentBoost = 8.0;  // 前方向（接線）ブースト
						constexpr double RadialBoost = 4.0;  // 外向き（半径）ブースト

						vel += tangent * (TangentBoost * sgn) + dirR * RadialBoost;
						vel.y -= JumpPower / 1.5;
					}
					isHooked = false;
				}

				// --- 通常の“巻取り/伸長”は、Wを押した瞬間のフレームはスキップ ---
				if (!didHookJump) {
					constexpr double ReelSpeed = 3.0;
					constexpr double RopeMinLen = 60.0;
					constexpr double RopeMaxLen = HookRange;

					double reel = ReelSpeed * (KeyShift.pressed() ? 3.0 : 3.0);
					if (Mouse::Wheel() >= 0) ropeRestLen = Max(RopeMinLen, ropeRestLen - reel); // 巻く
					if (Mouse::Wheel() <= 0) ropeRestLen = Min(RopeMaxLen, ropeRestLen + reel); // 伸ばす
				}

				// --- バネ力（ロープは“押さない”＝圧縮ゼロ化） ---
				if (isHooked) { // まだ掴んでいる場合だけ力を加える
					Vec2 r = pos - hookPoint;
					const double len = r.length();
					if (len > 1e-3) {
						const Vec2 dirR = r / len;
						const double stretch = Max(0.0, len - ropeRestLen);  // 伸びのみ
						const double vParallel = vel.dot(dirR);
						vel += (-Kspring * stretch) * dirR + (-Cdamp * vParallel) * dirR;

						// A/D で“こぐ”
						const int hdir = (KeyD.pressed() - KeyA.pressed());
						vel += Vec2(-dirR.y, dirR.x) * (hdir * 0.4);
					}
				}
			}

			// 武器切替：Q  
			// 武器切替：Q（次の武器） / E（前の武器） / ホイール / 数字キー
			if (KeyQ.down())
			{
				weaponIndex = (weaponIndex + 1) % weaponList.size();
				weapon = weaponList[weaponIndex];
				magSize = weaponMagSize[weaponIndex];
				ammoInMag = mag[weaponIndex];
			}

			// 実体を同期（必要なところで weapon を参照しているため）
			weapon = currentWeapon();
			magSize = weaponMagSize[weaponIndex];
			if (ammoInMag > magSize) ammoInMag = magSize;

			// リロード：R（シンプルに即時反映）
			if (KeyR.down()) {
				int need = magSize - ammoInMag;
				int take = Min(need, reserve);
				ammoInMag += take;
				reserve -= take;
				mag[weaponIndex] = ammoInMag;
			}

			// 経過時間を使う（v0.6.15 なら DeltaTime() あり）
			if (shootCooldown > 0.0) shootCooldown = Max(0.0, shootCooldown - dt);
			if (muzzleTimer > 0.0) { muzzleTimer = Max(0.0, muzzleTimer - dt); muzzleOn = (muzzleTimer > 0.0); }

			// 寿命更新（火花／弾痕／トレーサー）
			for (auto& im : impacts) im.t -= dt;
			impacts.remove_if([](const Impact& im) { return im.t <= 0.0; });

			for (auto& tr : tracers) tr.t -= dt;
			tracers.remove_if([](const Tracer& tr) { return tr.t <= 0.0; });


			const bool trigger = (weaponIsAuto[weaponIndex] ? MouseL.pressed() : MouseL.down());
			if (trigger) {
				const GunParams& g = paramsOf(currentWeapon());
				if (shootCooldown <= 0.0 && ammoInMag > 0) {
					const Vec2 mouseWorld = Vec2(Cursor::Pos()) + (cam - Vec2(WindowX / 2.0, WindowY / 2.0));
					const Vec2 baseDir = (mouseWorld - pos).length() > 0.0001 ? (mouseWorld - pos).normalized() : Vec2{ 1,0 };

					// 現在の拡散角（武器本来の拡散 + ブルーム）
					const double extra = (weapon == Weapon::Rifle ? 0.0 : 0.0); // 必要なら武器固有の追い足し
					const double currentSpread = g.spreadDeg + spreadBloomDeg + extra;

					if (weapon == Weapon::Rifle)
					{
						AR.stop();
						AR.play();
					}
					if (weapon == Weapon::Shotgun)
					{
						SG.stop();
						SG.play();
					}
					if (weapon == Weapon::SMG)
					{
						AR.stop();
						AR.play();
					}

					// 弾道可視化用：射撃ごとに1本（ライフル）/複数（SG）でもOK
					// ここではペレットごとにヒット点までの1本を積みます
					for (int i = 0; i < g.pellets; ++i) {
						const double spread = (g.pellets == 1) ? 0.0 : Random(-currentSpread, currentSpread);
						const Vec2 dir = withSpread(baseDir, spread);

						// 1) まず敵にヒット？
						Optional<EnemyHit> eh = raycastEnemy(pos, dir, g.range);

						Vec2 endPoint = pos + dir * g.range;

						if (eh) {
							// 命中点
							endPoint = eh->point;

							// ダメージ適用
							const int dmg = damage[weaponIndex];      // 既存のダメージ表
							auto& en = enemies[eh->index];
							en.hp = Max(0, en.hp - dmg);

							// ノックバック
							en.vel += dir * 3.5;

							// 命中エフェクト
							impacts << Impact{ endPoint, 0.20 };
						}
						else {
							// 2) 敵に当たらなければマップにヒット？
							if (auto hit = raycastBullet(pos, dir, g.range)) {
								endPoint = *hit;
								impacts << Impact{ *hit, 0.25 };
							}
						}

						// トレーサー
						tracers << Tracer{ pos, endPoint, 0.06 };
						for (int i = 0; i < g.pellets; ++i) {
							const double spread = (g.pellets == 1) ? 0.0 : Random(-currentSpread, currentSpread);
							const Vec2 dir = withSpread(baseDir, spread);

							// ... ここまであなたの既存の命中処理 ...

							// トレーサー
							tracers << Tracer{ pos, endPoint, 0.06 };

							// ★ ネットにもこの弾を知らせる
							// [startX, startY, dirX*1000, dirY*1000, shooterID]
							Array<int32> shotData = {
								(int32)pos.x,
								(int32)pos.y,
								(int32)(dir.x * 1000),
								(int32)(dir.y * 1000),
								network.m_myLocalID  // ← MyNetworkからpublicで取れるようにしてもいい
							};
							network.sendEvent(1, shotData);
						}
					}


					vel -= baseDir * (
						(weapon == Weapon::Rifle) ? 1.5 :
						(weapon == Weapon::Shotgun) ? 7.5 :
						0.5
					);
					muzzleOn = true; muzzleTimer = 0.05;
					ammoInMag -= 1;
					--mag[weaponIndex];
					shootCooldown = g.cooldown;
				}
			}




			// ===== 画像マップ衝突のみ（X→Y） =====
			// --- X ---
			// --- X ---
			Vec2 next = pos + Vec2{ vel.x, 0 };
			{
				const int samples = 5;                        // 上下にサンプル
				const double step = PlayerSize / (samples - 1);
				const int sgn = (vel.x > 0) ? +1 : (vel.x < 0 ? -1 : 0);
				if (sgn != 0) {
					bool hit = false;
					int safe = 0;                             // 無限ループ防止
					for (;;) {
						hit = false;
						for (int i = 0; i < samples; ++i) {
							const double yy = pos.y - Half + step * i;
							const double xx = next.x + sgn * Half;
							if (isSolidAt({ xx, yy }, 1)) { hit = true; break; }  // ← pad=1
						}
						if (!hit || ++safe > 64) break;
						next.x -= sgn;                        // 1px 押し戻し
					}
					if (hit) vel.x = 0;
				}
			}
			pos = next;

			// --- Y ---
			next = pos + Vec2{ 0, vel.y };
			onGround = false;
			{
				const int samples = 5;
				const double step = PlayerSize / (samples - 1);
				const int sgn = (vel.y > 0) ? +1 : (vel.y < 0 ? -1 : 0);
				if (sgn != 0) {
					bool anyHit = false;
					int safe = 0;
					for (;;) {
						bool hit = false;
						for (int i = 0; i < samples; ++i) {
							const double xx = pos.x - Half + step * i;
							const double yy = next.y + sgn * Half;
							if (isSolidAt({ xx, yy }, 1)) { hit = true; break; }
						}
						if (!hit || ++safe > 64) break;
						next.y -= sgn;
						anyHit = true;
					}
					if (anyHit) {
						vel.y = 0;                // ← 押し戻したら必ず速度をゼロに！
						onGround = (sgn > 0);     // ← 下方向に当たったら接地
					}
				}
			}
			pos = next;



			// ジャンプ
			if (onGround && (KeyW.down() || KeySpace.down() || KeyUp.down())) {
				vel.y = -JumpPower;
			}

		}
		
		// === 敵の簡易更新（重力のみ / 画像マップで床に乗せる）===
		for (auto& e : enemies) {
			if (!e.alive()) continue;
			e.vel.y += 0.6; // 簡易重力
			// Yだけ当たり：マップの isSolidAt を再利用（足元をサンプリング）
			Vec2 next = e.pos + Vec2{ 0, e.vel.y };
			bool grounded = false;
			const int samples = 3;
			const double stepY = e.w / (samples - 1);
			const int sgn = (e.vel.y > 0) ? +1 : (e.vel.y < 0 ? -1 : 0);
			if (sgn != 0) {
				bool hit = false; int safe = 0;
				for (;;) {
					hit = false;
					for (int i = 0; i < samples; ++i) {
						const double xx = e.pos.x - (e.w * 0.5) + stepY * i;
						const double yy = next.y + sgn * (e.h * 0.5);
						if (isSolidAt({ xx, yy }, 1)) { hit = true; break; }
					}
					if (!hit || ++safe > 64) break;
					next.y -= sgn;
				}
				if (hit) { e.vel.y = 0; grounded = (sgn > 0); }
			}
			e.pos = next;

			// 横は今回は動かさない（AIは後で）
		}


		// カメラ
		cam += (pos - cam) * 0.1;

		// === 描画 ===
		// マップはカメラ変換して描画（左上 mapOrigin 基準）
		// === マップ（角丸レクトで滑らかに描画） ===
		// === マップ（材質ごとのカラーパレットで角丸描画） ===
		for (const auto& t : mapTiles)
		{
			ColorF base, frame, highlight;
			switch (t.mat)
			{
			case TileMaterial::Red:
				base = ColorF(0.60, 0.18, 0.18);
				frame = ColorF(0.25, 0.06, 0.06, 0.55);
				highlight = ColorF(1.00, 0.45, 0.25, 0.08);
				break;
			case TileMaterial::Yellow:
				base = ColorF(0.78, 0.66, 0.28);
				frame = ColorF(0.35, 0.27, 0.05, 0.55);
				highlight = ColorF(1.00, 0.95, 0.50, 0.08);
				break;
			case TileMaterial::Gray:
				base = ColorF(0.28, 0.30, 0.34);
				frame = ColorF(0.05, 0.06, 0.08, 0.55);
				highlight = ColorF(1.00, 1.00, 1.00, 0.05);
				break;
			default:
				base = ColorF(t.avg).lerp(ColorF(0, 0, 0), 0.3);
				frame = ColorF(0, 0, 0, 0.45);
				highlight = ColorF(1, 1, 1, 0.05);
				break;
			}

			const double radius = 6.0;
			// ★ t.r を渡す！（TileRect→RectF）
			RoundRect rr(toScreenRect(t.r), radius);
			rr.draw(base);
			rr.drawFrame(2.0, 0.0, frame);

			RectF cap = t.r;
			cap.h = Min(8.0, t.r.h * 0.2);     // ← t.r.h
			RoundRect rrTop(toScreenRect(cap), radius);
			rrTop.draw(highlight);
		}

		// === 敵の描画 ===
		for (const auto& e : enemies) {
			if (!e.alive()) continue;
			const double ratio = Clamp((double)e.hp / e.hpMax, 0.0, 1.0);
			const RectF r = e.aabb();
			// 本体
			RoundRect rr(toScreenRect(r), 6.0);
			rr.draw(ColorF(0.25, 0.35, 0.55));
			rr.drawFrame(2, 0, ColorF(0, 0, 0, 0.45));
			// HPバー（頭上）
			const double barW = r.w, barH = 6.0;
			const Vec2 barPos = toScreen(r.tl() + Vec2{ 0, -10 });
			RoundRect(RectF(barPos, barW, barH), 3).draw(ColorF(0, 0, 0, 0.35));
			RoundRect(RectF(barPos, barW * ratio, barH), 3).draw(ColorF(0.9, 0.2, 0.2, 0.9));
		}


		// ===== エイム可視化（マップの手前レイヤに描画） =====
		{
			// 基本方向（プレイヤー→マウス）
			const Vec2 diff = (Vec2(Cursor::Pos()) + (cam - Vec2(WindowX / 2.0, WindowY / 2.0))) - pos;
			const Vec2 baseDir = (diff.length() > 0.0001) ? diff.normalized() : Vec2{ 1,0 };

			// 現在拡散角（視覚化用）
			const bool isShotgun = (weapon == Weapon::Shotgun);
			auto paramsOf = [&](Weapon w) -> const GunParams& {
				return (w == Weapon::Rifle) ? rifle
					: (w == Weapon::Shotgun) ? shotgun
					: smg;
				};

			// …同様に使用箇所も
			const GunParams& g = (weapon == Weapon::Rifle) ? rifle
				: (weapon == Weapon::Shotgun) ? shotgun
				: smg;

			const double currentSpread = g.spreadDeg + spreadBloomDeg;

			// 中心線のヒット点（“真っ直ぐ撃つとここに当たる”）
			Vec2 centerEnd = pos + baseDir * g.range;
			if (auto hit = raycastBullet(pos, baseDir, g.range)) centerEnd = *hit;

			// 予測点（SGは複数、Rifleは1点）を薄く描く
			const int previewCount = (isShotgun ? AimPreviewPellets : 1);
			for (int i = 0; i < previewCount; ++i) {
				double ang = (previewCount == 1) ? 0.0 : Random(-currentSpread, currentSpread);
				Vec2 d = withSpread(baseDir, ang);
				Vec2 end = pos + d * g.range;
				if (auto hit = raycastBullet(pos, d, g.range)) end = *hit;

				const double a = Clamp(AimPreviewAlpha, 0.0, 1.0);
				// 弾道を細く表示（プレビューなので薄め）
				Line(toScreen(pos), toScreen(end)).draw(1, ColorF(1.0, 0.95, 0.7, a * 0.6));
				// 命中点に小さな円
				Circle(toScreen(end), isShotgun ? 2.0 : 3.0).draw(ColorF(1.0, 0.9, 0.3, a));
			}

			// 拡散“コーン”の両端線（±currentSpread）を薄く
			if (currentSpread > 0.1) {
				Vec2 leftDir = withSpread(baseDir, +currentSpread);
				Vec2 rightDir = withSpread(baseDir, -currentSpread);
				const double len = Min(180.0, g.range * 0.4); // コーン線は手前だけ
				Line(toScreen(pos), toScreen(pos + leftDir * len)).draw(1, ColorF(0.9, 0.9, 0.9, 0.25));
				Line(toScreen(pos), toScreen(pos + rightDir * len)).draw(1, ColorF(0.9, 0.9, 0.9, 0.25));
			}

			// クロスヘア（ブルーム量で広がる）
			{
				const Vec2 m = Vec2(Cursor::Pos());
				const double r = 6 + currentSpread * 0.25; // 広がり
				const ColorF col(1.0, 1.0, 1.0, 0.7);
				// 4方向の短線
				Line(m + Vec2(-r - 4, 0), m + Vec2(-r, 0)).draw(2, col);
				Line(m + Vec2(+r, 0), m + Vec2(+r + 4, 0)).draw(2, col);
				Line(m + Vec2(0, -r - 4), m + Vec2(0, -r)).draw(2, col);
				Line(m + Vec2(0, +r), m + Vec2(0, +r + 4)).draw(2, col);
				// 中心点
				Circle(m, 1.5).draw(col);
			}
		}

		// 体力デバッグ：Kで-10 / Jで+10
		if (KeyK.down()) hp = Max(0, hp - 10);
		if (KeyJ.down()) hp = Min(hpMax, hp + 10);

		// 低HPビネット（画面に薄い赤を重ねる）
		{
			const double v = 1.0 - Clamp((double)hp / hpMax, 0.0, 1.0);
			if (v > 0.0) {
				Rect(Rect(0, 0, Scene::Size())).draw(ColorF(0.7, 0.05, 0.05, 0.18 * v));
			}
		}


		if (isHooked) {
			Line(toScreen(hookPoint), toScreen(pos)).draw(4, Palette::Limegreen);
			Circle(toScreen(hookPoint), 5).draw(Palette::Yellow);
		}

		// ★ 受信した他プレイヤーを描画
			// まずリモートプレイヤーを描く
		for (const auto& [pid, rpos] : network.remotePositions())
		{
			const Vec2 sp = toScreen(rpos);
			Circle(sp, 20).draw(Palette::Orange);

			// ★ もしこのプレイヤーがロープ中ならロープも描く
			if (const auto it = network.remoteRopes().find(pid); it != network.remoteRopes().end())
			{
				const auto& rr = it->second;
				if (rr.hooked)
				{
					const Vec2 ropeScreen = toScreen(rr.point);
					Line(sp, ropeScreen).draw(3, ColorF(0.8, 1.0, 1.0));
					Circle(ropeScreen, 6).draw(ColorF(1.0, 0.9, 0.3));
				}
			}
		}



		drawHUD();

		RectF(Arg::center(toScreen(pos)), PlayerSize, PlayerSize).draw(Palette::Skyblue);

		enemies.remove_if([](const Enemy& e) { return !e.alive(); });

		--hookCool;

		//if (SimpleGUI::Button(U"Connect", Vec2{ 1000, 20 }, 160, (not network.isActive())))
		//{
		//	const String userName = U"Player";
		//	network.connect(userName);
		//}

		//if (SimpleGUI::Button(U"Disconnect", Vec2{ 1000, 60 }, 160, network.isActive()))
		//{
		//	network.disconnect();
		//}

		//if (SimpleGUI::Button(U"Join Room", Vec2{ 1000, 100 }, 160, network.isInLobby()))
		//{
		//	network.joinRandomRoom(MyNetwork::MaxPlayers);
		//}

		if (g_gameState == GameState::SteelClimbings)
		{
			if (!g_match.inGame)
			{
				// カウントダウン表示
				const int disp = Max(1, (int)Ceil(g_match.countdown));
				RectF{ Arg::center = Scene::Center(), 260, 120 }
					.rounded(30)
					.draw(ColorF(0, 0, 0, 0.5));
				big(U"{}"_fmt(disp)).drawAt(Scene::Center().movedBy(0, -10), Palette::White);
			}
			else
			{
				// 開始直後だけ "START!"
				// 必要ならタイマーをもう1個持つ
			}
		}


		//オンライン対戦などのシステムのところ♡むやみに触るなよ★
		Point pointP = pos.asPoint(); // Point{ 3, 4 };
		Array<int32> playerVecs = { pointP.x, pointP.y }; // Array<int32>{ 3, 4 };
		network.sendEvent(0,playerVecs);
		ClearPrint();
	}
}
