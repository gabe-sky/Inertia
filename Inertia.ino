// Inertia: a tilt game for the M5Stack CoreS3
//
// Tilt the device and everything on screen feels it. Your puck, a little moon, is light: it
// picks up speed quickly and stops quickly. The coloured weights are heavy: they
// are slow to get going and slow to stop. Steer the puck into the star to
// score, without sloshing a weight into yourself. Every few stars another
// weight joins in. Touching a weight ends the run. Now and then a flying
// saucer crosses the screen; catch it and it takes one weight away.
//
// Touch:
//   tap   start a run
//   hold  re-set level (only while the device is lying roughly flat)
//   tap the speaker icon, bottom left, to step the sound: off, normal, loud
//
// The game is meant to be played lying flat on a surface or held flat like
// a tray, and tipped from there. Level is trimmed at power-on and by a hold,
// but only ever by a few degrees, so flat always stays flat.

#include <M5Unified.h>
#include <Preferences.h>
#include <math.h>
#include "sounds.h"  // the sound effects, made by tools/make_sounds.py

// ---- Settings ----------------------------------------------------------
// The puck (the "marble" feel chosen on the device).
static const float PUCK_ACCEL      = 1400.0f;  // pixels/sec^2 at full tilt
static const float PUCK_DRAG       = 1.5f;     // higher = stops sooner
static const float PUCK_BOUNCE     = 0.50f;    // speed kept after hitting an edge, 0..1
static const float PUCK_RADIUS     = 11.0f;
static const float MOON_CYCLE_SEC  = 90.0f;    // new moon to new moon; the puck is drawn as a moon

// The weights. Each one gets its own heaviness between these limits.
static const float WEIGHT_ACCEL_MIN = 300.0f;  // pixels/sec^2 at full tilt, heaviest
static const float WEIGHT_ACCEL_MAX = 520.0f;  // lightest
static const float WEIGHT_DRAG      = 0.5f;    // low, so they keep sliding
static const float WEIGHT_BOUNCE    = 0.60f;
static const float WEIGHT_RADIUS    = 17.0f;
static const int   MAX_WEIGHTS      = 6;
static const int   STARS_PER_WEIGHT = 4;       // a new weight arrives every this many stars
static const float WEIGHT_ARRIVE_SEC = 1.5f;   // a new weight is harmless while it fades in

// Tilt, measured in g (0 = level, 1 = on edge). 0.5 g is a 30 degree tilt.
static const float TILT_DEADZONE   = 0.035f; // below this nothing feels a push
static const float TILT_FULL       = 0.65f;  // at or above this the push is at full strength (about 40 degrees)
static const float LEVEL_TRIM_MAX  = 0.15f;  // setting level only works within this of truly flat (about 9 degrees)
static const float IDLE_HINT_SEC   = 5.0f;   // a run left untilted this long shows a hint
static const float TILT_SMOOTH_SEC = 0.04f;  // smoothing of hand shake; higher = calmer but laggier

// Colours a weight can spawn in, after Kandinsky's concentric circles and
// Calder's flat primaries. `rim` is the disc; `core` is a contrasting disc
// in its centre, which is bigger the heavier the weight is.
struct WeightPalette {
  uint8_t rim[3];
  uint8_t core[3];
};
static const WeightPalette WEIGHT_PALETTES[] = {
  {{216,  40,  30}, { 22,  22,  28}},  // vermilion, black centre
  {{246, 192,  18}, {216,  40,  30}},  // chrome yellow, vermilion centre
  {{ 22,  22,  28}, {246, 192,  18}},  // black, yellow centre
  {{112,  62, 152}, {246, 192,  18}},  // violet, yellow centre
  {{ 28, 124,  86}, {238, 150, 160}},  // viridian green, pink centre
  {{236, 120,  24}, { 26,  40,  92}},  // orange, navy centre
};
static const int WEIGHT_PALETTE_COUNT = sizeof(WEIGHT_PALETTES) / sizeof(WEIGHT_PALETTES[0]);

// The flying saucer: every so often it crosses the screen, ignoring the
// tilt. Catching it with the puck removes one weight.
static const float SAUCER_SPEED       = 65.0f;  // pixels/sec; about 4 seconds to cross
static const float SAUCER_WAIT_MIN    = 18.0f;  // seconds between visits, shortest
static const float SAUCER_WAIT_MAX    = 32.0f;  // and longest
static const int   SAUCER_MIN_WEIGHTS = 2;      // it only comes when there are at least this many
static const float SAUCER_REACH       = 13.0f;  // how close the puck's edge must get to its centre

static const float STAR_RADIUS     = 16.0f;
static const int   TRAIL_LENGTH    = 14;     // frames of trail behind the puck
static const float PHYSICS_STEP    = 1.0f / 120.0f;  // seconds; several steps per frame
static const uint8_t SCREEN_BRIGHTNESS = 160;

// Sound.
static const uint8_t SOUND_VOLUMES[] = {0, 80, 160};  // loudness when off, normal and loud; 0..255
static const float PUCK_TICK_MIN   = 40.0f;  // edge hits slower than this (pixels/sec) are silent
static const float WEIGHT_THUD_MIN = 60.0f;  // the same for weights hitting an edge or each other
static const int   SOUND_TAP_SIZE  = 44;     // the bottom-left square that takes the sound tap, pixels

// ---- State -------------------------------------------------------------
static M5Canvas canvas(&M5.Display);
static int screenW = 240;
static int screenH = 320;

static bool gameOver = false;
static bool beforeFirstRun = true;        // showing the title, waiting for the first tap
static uint32_t gameOverMs = 0;
static int score = 0;
static int bestScore = 0;                 // best this session; not saved across power-off

static float sensorX = 0, sensorY = 0;    // smoothed sensor reading, screen axes (x right, y down)
static float levelX = 0, levelY = 0;      // the reading that counts as level; never far from zero
static float idleSec = 0;                 // how long the current run has gone without any tilt
static float pushX = 0, pushY = 0;        // this frame's push on everything, each -1..1

static float puckX, puckY;                // centre, pixels
static float puckVX = 0, puckVY = 0;      // pixels per second
static float puckFlash = 0;               // 1 just after hitting an edge, fading to 0
static float moonPhase = 0.12f;           // 0 = new, 0.25 = first quarter, 0.5 = full, 0.75 = last quarter
static float trailX[TRAIL_LENGTH], trailY[TRAIL_LENGTH];
static int   trailHead = 0;

struct Weight {
  float x, y, vx, vy;
  float accel;     // pixels/sec^2 at full tilt; lower = heavier
  float arriving;  // seconds until it becomes solid and dangerous
  uint8_t palette; // which of WEIGHT_PALETTES it is drawn in
};
static Weight weights[MAX_WEIGHTS];
static int weightCount = 0;
static uint8_t caughtByPalette = 0;       // colours of the weight that ended the run

static bool  saucerFlying = false;
static float saucerX, saucerY;            // centre, pixels
static float saucerPathY;                 // the height it flies along; it bobs around this
static float saucerDir;                   // +1 flying right, -1 flying left
static float saucerWait = 0;              // seconds until the next visit

// The weight most recently taken by the saucer, shown shrinking away.
static float vanishX, vanishY;
static uint8_t vanishPalette = 0;
static float vanishLeft = 0;              // 1 just after it is taken, falling to 0

static float starX, starY;
static float starFlash = 0;               // 1 just after a pickup, fading to 0
static float flashX = 0, flashY = 0;      // where that pickup happened

static const char* notice = "";           // short message shown mid-screen
static uint32_t noticeUntilMs = 0;

static uint32_t lastFrameMs = 0;
static float physicsDebt = 0;             // seconds of physics not yet simulated
static uint32_t fpsWindowStart = 0;
static uint32_t fpsFrames = 0;
static int fps = 0;
static uint32_t lastPrintMs = 0;

// A random number from 0 to 1.
static float frand() { return (esp_random() & 0xFFFF) / 65535.0f; }

// ---- Sound -------------------------------------------------------------
// The effects are short recorded samples (see sounds.h). Each kind of sound
// has its own speaker channel, so they overlap freely, and starting one never
// waits on the speaker.
static const int FX_CHANNEL = 0;      // stars, start, end of run, saucer caught
static const int TICK_CHANNEL = 1;    // the puck on an edge
static const int THUD_CHANNEL = 2;    // weights on an edge or each other
static const int SAUCER_CHANNEL = 3;  // the saucer's twinkle

static Preferences settings;              // remembers the sound level across power-off
static uint8_t soundLevel = 1;             // 0 = off, 1 = normal, 2 = loud
static bool saucerHumming = false;        // whether the saucer's twinkle is playing
static float puckTick = 0;                // hardest puck edge hit this frame, pixels/sec
static float weightThud = 0;              // hardest weight hit this frame, pixels/sec

// Plays a sample, cutting off whatever its channel was playing. `pitch`
// speeds it up (above 1) or slows it down; `repeats` of 0 loops it until
// the channel is stopped.
static void playSound(const Sound& sound, int channel, float pitch = 1.0f, uint32_t repeats = 1) {
  if (soundLevel == 0) return;
  M5.Speaker.playRaw(sound.samples, sound.count, (uint32_t)(SOUND_RATE * pitch), false,
                     repeats ? repeats : UINT32_MAX, channel, true);
}

// The chime for a star. It climbs with each star towards the next weight;
// the star that brings a weight has a warning under it.
static void playStarChime(int starsSoFar, bool weightComing) {
  static const Sound* const CHIMES[] = {&SND_STAR_0, &SND_STAR_1, &SND_STAR_2, &SND_STAR_3};
  if (weightComing) {
    playSound(SND_STAR_WEIGHT, FX_CHANNEL);
    return;
  }
  int step = (starsSoFar + STARS_PER_WEIGHT - 1) % STARS_PER_WEIGHT;
  playSound(*CHIMES[step < 4 ? step : 3], FX_CHANNEL);
}

// Call once per frame: plays this frame's tick and thud, and keeps the
// saucer's twinkle going for as long as the saucer is on screen.
static void updateSound() {
  if (soundLevel > 0) {
    // Harder hits are louder and, for the puck, a little higher.
    if (puckTick > PUCK_TICK_MIN) {
      float hard = fminf(puckTick / 500.0f, 1.0f);
      M5.Speaker.setChannelVolume(TICK_CHANNEL, 60 + 195 * hard);
      playSound(SND_TICK, TICK_CHANNEL, 0.8f + 0.5f * hard);
    }
    if (weightThud > WEIGHT_THUD_MIN) {
      float hard = fminf(weightThud / 300.0f, 1.0f);
      M5.Speaker.setChannelVolume(THUD_CHANNEL, 70 + 185 * hard);
      playSound(SND_THUD, THUD_CHANNEL, 1.1f - 0.25f * hard);
    }
  }
  bool shouldHum = soundLevel > 0 && saucerFlying && !gameOver;
  if (shouldHum && !saucerHumming) playSound(SND_SAUCER_LOOP, SAUCER_CHANNEL, 1.0f, 0);
  if (!shouldHum && saucerHumming) M5.Speaker.stop(SAUCER_CHANNEL);
  saucerHumming = shouldHum;
  puckTick = weightThud = 0;
}

// Sets the sound to off (0), normal (1) or loud (2) and remembers it. A
// blip at the new loudness confirms the change.
static void setSoundLevel(uint8_t level) {
  soundLevel = level;
  settings.putUChar("level", level);
  M5.Speaker.setVolume(SOUND_VOLUMES[level]);
  if (level > 0) {
    playSound(SND_UNMUTE, FX_CHANNEL);
  } else {
    M5.Speaker.stop();
    saucerHumming = false;
  }
}

static void showNotice(const char* text, uint32_t now, uint32_t forMs) {
  notice = text;
  noticeUntilMs = now + forMs;
}

// Reads the raw sensor as screen-axis gravity: x to the right, y down.
static bool readSensor(float& gx, float& gy) {
  float ax = 0, ay = 0, az = 0;
  if (!M5.Imu.update() || !M5.Imu.getAccel(&ax, &ay, &az)) return false;
  gx = -ax;
  gy = ay;
  return true;
}

// Updates the smoothed tilt and works out the push on everything.
static void readTilt(float dt) {
  float gx, gy;
  if (readSensor(gx, gy)) {
    float k = 1.0f - expf(-dt / TILT_SMOOTH_SEC);
    sensorX += (gx - sensorX) * k;
    sensorY += (gy - sensorY) * k;
  }
  float tx = sensorX - levelX, ty = sensorY - levelY;
  float size = sqrtf(tx * tx + ty * ty);
  float strength = (size - TILT_DEADZONE) / (TILT_FULL - TILT_DEADZONE);
  if (strength <= 0) {
    pushX = pushY = 0;
    return;
  }
  if (strength > 1) strength = 1;
  pushX = tx / size * strength;
  pushY = ty / size * strength;
}

// Makes the current resting position count as level, to trim out a sloping
// desk or a slightly off sensor. Refused (returns false) if the device is
// tilted well away from flat, so being held upright can never become level.
static bool setLevel() {
  if (sensorX * sensorX + sensorY * sensorY > LEVEL_TRIM_MAX * LEVEL_TRIM_MAX) return false;
  levelX = sensorX;
  levelY = sensorY;
  return true;
}

// Picks a random spot at least `margin` from the edges and, if it can, at
// least `clear` pixels from the puck.
static void pickSpot(float margin, float clear, float& x, float& y) {
  for (int attempt = 0; attempt < 20; attempt++) {
    x = margin + frand() * (screenW - 2 * margin);
    y = margin + 12 + frand() * (screenH - 2 * margin - 24);  // clear of the text rows
    float dx = x - puckX, dy = y - puckY;
    if (dx * dx + dy * dy > clear * clear) return;
  }
}

static void placeStar() { pickSpot(STAR_RADIUS + 14, 90, starX, starY); }

static void addWeight() {
  if (weightCount >= MAX_WEIGHTS) return;
  Weight& w = weights[weightCount++];
  pickSpot(WEIGHT_RADIUS + 4, 110, w.x, w.y);
  w.vx = w.vy = 0;
  w.accel = WEIGHT_ACCEL_MIN + frand() * (WEIGHT_ACCEL_MAX - WEIGHT_ACCEL_MIN);
  w.arriving = WEIGHT_ARRIVE_SEC;

  // A random palette, avoiding ones already on screen while any are unused.
  w.palette = esp_random() % WEIGHT_PALETTE_COUNT;
  for (int attempt = 0; attempt < WEIGHT_PALETTE_COUNT; attempt++) {
    bool used = false;
    for (int i = 0; i < weightCount - 1; i++) used |= weights[i].palette == w.palette;
    if (!used) break;
    w.palette = (w.palette + 1) % WEIGHT_PALETTE_COUNT;
  }
}

static void startRun() {
  gameOver = false;
  score = 0;
  puckX = screenW / 2;
  puckY = screenH / 2;
  puckVX = puckVY = 0;
  puckFlash = starFlash = 0;
  for (int i = 0; i < TRAIL_LENGTH; i++) { trailX[i] = puckX; trailY[i] = puckY; }
  weightCount = 0;
  addWeight();
  idleSec = 0;
  saucerFlying = false;
  saucerWait = SAUCER_WAIT_MIN + frand() * (SAUCER_WAIT_MAX - SAUCER_WAIT_MIN);
  vanishLeft = 0;
  placeStar();
}

// Reverses one velocity component after hitting an edge. Returns how hard
// the hit was, in pixels per second.
static float bounceOff(float& v, float towardsInside, float bounce) {
  float impact = fabsf(v);
  v = impact * bounce * towardsInside;
  return impact;
}

// Moves one round thing under the shared tilt and bounces it off the edges.
// Returns the hardest edge hit this step, in pixels per second.
static float moveBody(float& x, float& y, float& vx, float& vy, float radius, float accel,
                      float drag, float bounce, float dt) {
  vx += pushX * accel * dt;
  vy += pushY * accel * dt;
  float keep = expf(-drag * dt);
  vx *= keep;
  vy *= keep;
  x += vx * dt;
  y += vy * dt;

  float impact = 0;
  if (x < radius)           { x = radius;           impact = fmaxf(impact, bounceOff(vx,  1, bounce)); }
  if (x > screenW - radius) { x = screenW - radius; impact = fmaxf(impact, bounceOff(vx, -1, bounce)); }
  if (y < radius)           { y = radius;           impact = fmaxf(impact, bounceOff(vy,  1, bounce)); }
  if (y > screenH - radius) { y = screenH - radius; impact = fmaxf(impact, bounceOff(vy, -1, bounce)); }
  return impact;
}

// Makes solid weights knock into each other. They all weigh the same here.
static void collideWeights() {
  for (int i = 0; i < weightCount; i++) {
    Weight& a = weights[i];
    if (a.arriving > 0) continue;
    for (int j = i + 1; j < weightCount; j++) {
      Weight& b = weights[j];
      if (b.arriving > 0) continue;
      float dx = b.x - a.x, dy = b.y - a.y;
      float dist2 = dx * dx + dy * dy;
      float minDist = 2 * WEIGHT_RADIUS;
      if (dist2 >= minDist * minDist || dist2 < 0.0001f) continue;
      float dist = sqrtf(dist2);
      float nx = dx / dist, ny = dy / dist;
      float overlap = (minDist - dist) * 0.5f;
      a.x -= nx * overlap; a.y -= ny * overlap;
      b.x += nx * overlap; b.y += ny * overlap;
      float closing = -((b.vx - a.vx) * nx + (b.vy - a.vy) * ny);
      if (closing <= 0) continue;
      weightThud = fmaxf(weightThud, closing);
      float impulse = (1.0f + WEIGHT_BOUNCE) * closing * 0.5f;
      a.vx -= nx * impulse; a.vy -= ny * impulse;
      b.vx += nx * impulse; b.vy += ny * impulse;
    }
  }
}

// Sends the saucer across now and then, and lets the puck catch it.
static void updateSaucer(float dt) {
  const float offScreen = 22;  // far enough past an edge to be out of sight
  if (!saucerFlying) {
    // The wait only counts down while there are enough weights to spare one.
    if (weightCount < SAUCER_MIN_WEIGHTS) return;
    saucerWait -= dt;
    if (saucerWait > 0) return;
    saucerFlying = true;
    saucerDir = (esp_random() & 1) ? 1 : -1;
    saucerX = saucerDir > 0 ? -offScreen : screenW + offScreen;
    saucerPathY = 50 + frand() * (screenH - 100);
  }

  saucerX += saucerDir * SAUCER_SPEED * dt;
  saucerY = saucerPathY + 8 * sinf(saucerX / 18.0f);  // a gentle bob
  bool caught = false;
  float dx = saucerX - puckX, dy = saucerY - puckY;
  float reach = SAUCER_REACH + PUCK_RADIUS;
  if (dx * dx + dy * dy < reach * reach && weightCount > 0) {
    // Caught: it takes away the weight nearest the puck.
    int nearest = 0;
    float nearestDist2 = 1e12f;
    for (int i = 0; i < weightCount; i++) {
      float wx = weights[i].x - puckX, wy = weights[i].y - puckY;
      if (wx * wx + wy * wy < nearestDist2) { nearestDist2 = wx * wx + wy * wy; nearest = i; }
    }
    vanishX = weights[nearest].x;
    vanishY = weights[nearest].y;
    vanishPalette = weights[nearest].palette;
    vanishLeft = 1;
    weights[nearest] = weights[--weightCount];
    caught = true;
    playSound(SND_CAUGHT, FX_CHANNEL);
  }

  if (caught || saucerX < -offScreen || saucerX > screenW + offScreen) {
    saucerFlying = false;
    saucerWait = SAUCER_WAIT_MIN + frand() * (SAUCER_WAIT_MAX - SAUCER_WAIT_MIN);
  }
}

static void stepPhysics(float dt, uint32_t now) {
  float impact = moveBody(puckX, puckY, puckVX, puckVY, PUCK_RADIUS, PUCK_ACCEL, PUCK_DRAG,
                          PUCK_BOUNCE, dt);
  puckTick = fmaxf(puckTick, impact);
  if (impact > 40.0f) {
    float flash = impact / 300.0f;
    if (flash > 1) flash = 1;
    if (flash > puckFlash) puckFlash = flash;
  }

  for (int i = 0; i < weightCount; i++) {
    Weight& w = weights[i];
    if (w.arriving > 0) {
      w.arriving -= dt;  // still fading in: it stays put and can't hurt
      continue;
    }
    weightThud = fmaxf(weightThud, moveBody(w.x, w.y, w.vx, w.vy, WEIGHT_RADIUS, w.accel,
                                            WEIGHT_DRAG, WEIGHT_BOUNCE, dt));
  }
  collideWeights();

  // Touching a solid weight ends the run. The reach is a little short of the
  // drawn sizes so a near miss counts as a miss.
  for (int i = 0; i < weightCount; i++) {
    const Weight& w = weights[i];
    if (w.arriving > 0) continue;
    float dx = w.x - puckX, dy = w.y - puckY;
    float reach = WEIGHT_RADIUS + PUCK_RADIUS - 3;
    if (dx * dx + dy * dy < reach * reach) {
      gameOver = true;
      gameOverMs = now;
      caughtByPalette = w.palette;
      // A new best gets a fanfare after the crash.
      playSound(score > bestScore ? SND_GAME_OVER_BEST : SND_GAME_OVER, FX_CHANNEL);
      if (score > bestScore) bestScore = score;
      return;
    }
  }

  updateSaucer(dt);

  float dx = starX - puckX, dy = starY - puckY;
  float reach = STAR_RADIUS + PUCK_RADIUS * 0.5f;  // the puck must be well into the star
  if (dx * dx + dy * dy < reach * reach) {
    score++;
    starFlash = 1;
    flashX = starX;
    flashY = starY;
    placeStar();
    bool weightComing = score % STARS_PER_WEIGHT == 0 && weightCount < MAX_WEIGHTS;
    if (weightComing) addWeight();
    playStarChime(score, weightComing);
  }
}

// A colour between the background and `rgb`; amount 0 = background, 1 = rgb.
static uint16_t shade(const uint8_t rgb[3], float amount) {
  static const uint8_t bg[3] = {242, 234, 214};  // cream paper
  if (amount < 0) amount = 0;
  if (amount > 1) amount = 1;
  return lgfx::color565(bg[0] + (rgb[0] - bg[0]) * amount,
                        bg[1] + (rgb[1] - bg[1]) * amount,
                        bg[2] + (rgb[2] - bg[2]) * amount);
}

// Fills a five-pointed star centred on (cx, cy). `radius` is to the tips and
// `angle` turns it, in radians; at 0 one tip points straight up.
static void fillStar(float cx, float cy, float radius, float angle, uint16_t color) {
  float px[10], py[10];
  for (int i = 0; i < 10; i++) {
    float r = (i & 1) ? radius * 0.45f : radius;  // odd corners are the inner ones
    float a = angle - HALF_PI + i * PI / 5;
    px[i] = cx + cosf(a) * r;
    py[i] = cy + sinf(a) * r;
  }
  for (int i = 0; i < 10; i += 2) {
    int before = (i + 9) % 10, after = i + 1;
    // One tip, then the slice of the middle behind it.
    canvas.fillTriangle(px[before], py[before], px[i], py[i], px[after], py[after], color);
    canvas.fillTriangle(cx, cy, px[before], py[before], px[after], py[after], color);
    canvas.fillTriangle(cx, cy, px[after], py[after], px[(i + 3) % 10], py[(i + 3) % 10], color);
  }
}

// Draws text in `color` with a 2-pixel `halo` colour around every letter, so
// it stays readable over anything. Uses the canvas's current font and datum.
static void drawHaloText(const String& text, int x, int y, uint16_t color, uint16_t halo) {
  canvas.setTextColor(halo);
  for (int dy = -2; dy <= 2; dy++) {
    for (int dx = -2; dx <= 2; dx++) {
      if ((dx == 0 && dy == 0) || (abs(dx) == 2 && abs(dy) == 2)) continue;  // rounded corners
      canvas.drawString(text, x + dx, y + dy);
    }
  }
  canvas.setTextColor(color);
  canvas.drawString(text, x, y);
}

static void draw(uint32_t now) {
  static const uint8_t PUCK[3]   = {24, 72, 190};    // cobalt blue; no weight is blue
  static const uint8_t PUCK_HIT[3] = {8, 20, 70};    // the puck's colour just after an edge hit
  static const uint8_t MOONLIGHT[3] = {255, 252, 240};  // the sunlit part of the moon
  static const uint8_t INK[3]    = {22, 22, 28};     // black, for lines and big text
  static const uint8_t STAR[3]   = {246, 192, 18};   // chrome yellow, outlined in black
  static const uint8_t TEXT[3]   = {104, 96, 84};

  canvas.fillScreen(shade(PUCK, 0));

  // The burst left behind by the last pickup.
  if (starFlash > 0.02f) {
    float r = STAR_RADIUS + (1 - starFlash) * 26;
    canvas.drawCircle((int)flashX, (int)flashY, (int)r, shade(INK, starFlash));
  }

  // The star to steer into: slowly turning and gently swelling. A slightly
  // bigger black star behind the yellow one gives it an outline.
  float turn = now / 1400.0f;
  float size = STAR_RADIUS + 1.5f * sinf(now / 220.0f);
  fillStar(starX, starY, size + 2.5f, turn, shade(INK, 1));
  fillStar(starX, starY, size - 1.0f, turn, shade(STAR, 1));

  // Weights. One that is still arriving is a blinking warning sign; a solid one
  // is a flat disc with a black outline and a contrasting centre, which is
  // bigger the heavier it is.
  for (int i = 0; i < weightCount; i++) {
    const Weight& w = weights[i];
    const WeightPalette& colors = WEIGHT_PALETTES[w.palette];
    int x = (int)w.x, y = (int)w.y, r = (int)WEIGHT_RADIUS;
    if (w.arriving > 0) {
      // The weight is "falling" onto the screen: its shadow starts as a
      // speck and grows, darkening, to the weight's full size. A warning
      // triangle with an exclamation mark blinks inside it, growing too.
      float progress = 1 - w.arriving / WEIGHT_ARRIVE_SEC;  // 0 = just announced, 1 = landing
      if (progress < 0) progress = 0;
      if (progress > 1) progress = 1;
      float shadow = 3 + (r - 3) * progress;
      canvas.fillSmoothCircle(x, y, (int)shadow, shade(INK, 0.10f + 0.16f * progress));
      if ((now / 150) % 2 == 0) {
        uint16_t line = shade(INK, 0.55f);
        float s = shadow * 0.72f;  // the triangle's size, kept inside the shadow
        canvas.drawTriangle(x, y - s, x - s * 0.9f, y + s * 0.6f, x + s * 0.9f, y + s * 0.6f, line);
        if (s >= 6) {  // any smaller and the mark would be a smudge
          int wide = s >= 10 ? 3 : 1;
          canvas.fillRect(x - wide / 2, y - s * 0.45f, wide, s * 0.55f, line);        // the mark's stroke
          canvas.fillRect(x - wide / 2, y + s * 0.25f, wide, wide > 1 ? 3 : 2, line); // and its dot
        }
      }
      continue;
    }
    float lightness = (w.accel - WEIGHT_ACCEL_MIN) / (WEIGHT_ACCEL_MAX - WEIGHT_ACCEL_MIN);
    canvas.fillSmoothCircle(x, y, r, shade(INK, 1));
    canvas.fillSmoothCircle(x, y, r - 1, shade(colors.rim, 1));
    canvas.fillSmoothCircle(x, y, (int)(r * (0.30f + 0.35f * (1 - lightness))), shade(colors.core, 1));
  }

  // A weight the saucer has just taken: its disc shrinks to nothing inside
  // a widening circle.
  if (vanishLeft > 0.02f) {
    const uint8_t* rim = WEIGHT_PALETTES[vanishPalette].rim;
    canvas.fillSmoothCircle((int)vanishX, (int)vanishY, (int)(WEIGHT_RADIUS * vanishLeft), shade(rim, 1));
    canvas.drawCircle((int)vanishX, (int)vanishY, (int)(WEIGHT_RADIUS + (1 - vanishLeft) * 22),
                      shade(INK, vanishLeft));
  }

  // The flying saucer: a white dome behind a grey body, outlined in black,
  // with three lights that blink in turn.
  if (saucerFlying) {
    static const uint8_t SAUCER[3] = {150, 154, 160};
    static const uint8_t DOME[3]   = {255, 252, 240};
    static const uint8_t LIGHT[3]  = {246, 192, 18};
    int sx = (int)saucerX, sy = (int)saucerY;
    canvas.fillSmoothCircle(sx, sy - 3, 8, shade(INK, 1));
    canvas.fillSmoothCircle(sx, sy - 3, 7, shade(DOME, 1));
    canvas.fillEllipse(sx, sy + 2, 17, 7, shade(INK, 1));
    canvas.fillEllipse(sx, sy + 2, 16, 6, shade(SAUCER, 1));
    int litOne = (now / 160) % 3;
    for (int i = 0; i < 3; i++) {
      canvas.fillCircle(sx + (i - 1) * 9, sy + 2, 2, shade(i == litOne ? LIGHT : INK, 1));
    }
  }

  // Trail, oldest first: shows where the momentum has been carrying the puck.
  for (int i = 0; i < TRAIL_LENGTH; i++) {
    int slot = (trailHead + i) % TRAIL_LENGTH;
    float age = (i + 1) / (float)(TRAIL_LENGTH + 1);  // 0 = oldest, 1 = newest
    canvas.fillCircle((int)trailX[slot], (int)trailY[slot],
                      (int)(2 + (PUCK_RADIUS - 4) * age), shade(PUCK, 0.08f + 0.34f * age * age));
  }

  // A line from the puck showing which way, and how hard, the tilt is pushing.
  if (!gameOver && (pushX != 0 || pushY != 0)) {
    canvas.drawLine((int)puckX, (int)puckY, (int)(puckX + pushX * 46), (int)(puckY + pushY * 46),
                    shade(INK, 0.55f));
  }

  // The puck is a moon: a black outline, a blue disc for the dark side, and
  // the sunlit part drawn over it row by row. The blue darkens briefly after
  // an edge hit.
  uint8_t body[3];
  for (int c = 0; c < 3; c++) body[c] = PUCK[c] + (PUCK_HIT[c] - PUCK[c]) * puckFlash;
  int mx = (int)puckX, my = (int)puckY;
  canvas.fillSmoothCircle(mx, my, (int)PUCK_RADIUS, shade(INK, 1));
  canvas.fillSmoothCircle(mx, my, (int)PUCK_RADIUS - 1, shade(body, 1));
  // The edge of the sunlit part is half an ellipse whose width follows the
  // phase: +1 = new moon, 0 = half moon, -1 = full moon.
  float edge = cosf(moonPhase * TWO_PI);
  bool waxing = moonPhase < 0.5f;  // waxing is lit on the right, waning on the left
  float litRadius = PUCK_RADIUS - 1.5f;
  uint16_t lit = shade(MOONLIGHT, 1);
  for (int dy = -(int)litRadius; dy <= (int)litRadius; dy++) {
    float half = sqrtf(litRadius * litRadius - dy * dy);
    float x0 = waxing ? edge * half : -half;
    float x1 = waxing ? half : -edge * half;
    int left = (int)roundf(x0), right = (int)roundf(x1);
    if (right > left) canvas.drawFastHLine(mx + left, my + dy, right - left, lit);
  }

  // Text rows.
  canvas.setFont(&fonts::Font0);
  canvas.setTextColor(shade(TEXT, 1));
  canvas.setTextDatum(top_left);
  canvas.drawString(String(score) + " stars", 4, 4);
  canvas.setTextDatum(top_right);
  canvas.drawString("best " + String(bestScore), screenW - 4, 4);

  // The sound icon, bottom left: a little speaker with one sound wave for
  // normal, two for loud, or with a cross when off.
  {
    uint16_t c = shade(TEXT, 1);
    int x = 6, y = screenH - 13;                                 // the speaker's left edge and mid-height
    canvas.fillRect(x, y - 3, 4, 7, c);                          // the box
    canvas.fillTriangle(x + 2, y, x + 9, y - 7, x + 9, y + 7, c);  // the cone
    if (soundLevel > 0) {
      for (int wave = 0; wave < soundLevel; wave++) {
        int wx = x + 13 + wave * 4, half = 2 + wave * 2;
        canvas.drawFastVLine(wx, y - half, half * 2 + 1, c);
        canvas.drawLine(wx, y - half, wx - 2, y - half - 3, c);
        canvas.drawLine(wx, y + half, wx - 2, y + half + 3, c);
      }
    } else {
      canvas.drawLine(x + 12, y - 3, x + 18, y + 3, c);
      canvas.drawLine(x + 12, y + 3, x + 18, y - 3, c);
    }
  }

  if (gameOver) {
    // Expanding circle where the puck was caught, then the result.
    float since = (now - gameOverMs) / 1000.0f;
    if (since < 0.6f && !beforeFirstRun) {
      canvas.drawCircle((int)puckX, (int)puckY, (int)(PUCK_RADIUS + since * 120),
                        shade(WEIGHT_PALETTES[caughtByPalette].rim, 1 - since / 0.6f));
    }
    canvas.setTextDatum(middle_center);
    // Black letters with a cream halo, so they read over weights of any colour.
    uint16_t ink = shade(INK, 1), paper = shade(INK, 0);
    canvas.setFont(&fonts::Font4);
    String headline = String(score) + (score == 1 ? " star" : " stars");
    const char* prompt = score > 0 && score >= bestScore ? "best so far - tap to start" : "tap to start again";
    if (beforeFirstRun) {
      headline = "Inertia";
      prompt = "lay it flat - tap to start";
    }
    drawHaloText(headline, screenW / 2, screenH / 2 - 16,
                 ink, paper);
    canvas.setFont(&fonts::Font2);
    drawHaloText(prompt, screenW / 2, screenH / 2 + 16, ink, paper);
    drawHaloText("tilt gently to move", screenW / 2, screenH / 2 + 36, ink, paper);
  }
  // Nobody has tilted the device for a while: say what to do, until they do.
  if (idleSec >= IDLE_HINT_SEC) {
    canvas.setFont(&fonts::Font2);
    canvas.setTextDatum(middle_center);
    drawHaloText("tilt gently - collect stars", screenW / 2, screenH / 2 - 40, shade(INK, 1), shade(INK, 0));
  }
  if (now < noticeUntilMs) {
    canvas.setFont(&fonts::Font2);
    canvas.setTextDatum(middle_center);
    drawHaloText(notice, screenW / 2, screenH / 2 + 62, shade(INK, 1), shade(INK, 0));
  }

  canvas.pushSprite(0, 0);
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);

  settings.begin("inertia");
  // Before there were levels this was an on/off choice saved as "sound".
  soundLevel = settings.getUChar("level", settings.getBool("sound", true) ? 1 : 0);
  if (soundLevel > 2) soundLevel = 1;
  M5.Speaker.begin();
  M5.Speaker.setVolume(SOUND_VOLUMES[soundLevel]);

  M5.Display.setRotation(0);  // portrait, 240x320
  M5.Display.setColorDepth(16);
  M5.Display.setBrightness(SCREEN_BRIGHTNESS);
  M5.Display.fillScreen(TFT_BLACK);
  screenW = M5.Display.width();
  screenH = M5.Display.height();

  // A full-screen 16-bit canvas is about 154 KB, so prefer PSRAM and fall
  // back to internal RAM.
  canvas.setColorDepth(16);
  canvas.setPsram(true);
  if (!canvas.createSprite(screenW, screenH)) {
    canvas.setPsram(false);
    canvas.createSprite(screenW, screenH);
  }
  if (!canvas.getBuffer()) {
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::Font2);
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.drawString("Not enough memory", screenW / 2, screenH / 2);
    Serial.println("Canvas allocation failed");
    while (true) delay(1000);
  }

  // Trim level from how the device is resting at power-on: average the
  // sensor for a quarter of a second. If it isn't roughly flat, level stays
  // at truly flat.
  float sumX = 0, sumY = 0;
  int samples = 0;
  for (int i = 0; i < 25; i++) {
    float gx, gy;
    if (readSensor(gx, gy)) { sumX += gx; sumY += gy; samples++; }
    delay(10);
  }
  if (samples > 0) {
    sensorX = sumX / samples;
    sensorY = sumY / samples;
  }
  setLevel();

  // Lay out a field, but wait on the title for a tap before anything moves.
  startRun();
  gameOver = true;
  gameOverMs = 0;
  lastFrameMs = millis();
  fpsWindowStart = lastFrameMs;
}

void loop() {
  M5.update();

  uint32_t now = millis();
  float dt = (now - lastFrameMs) / 1000.0f;
  if (dt > 0.1f) dt = 0.1f;  // don't jump after a stall
  lastFrameMs = now;

  auto touch = M5.Touch.getDetail();
  // A short wait before a tap restarts, so a stray touch at the moment of
  // the crash doesn't skip the result.
  // Each run starts from a tap, so nothing moves until the player is ready.
  // A tap on the sound icon's corner only steps the sound: off, normal, loud.
  if (touch.wasClicked() && touch.x < SOUND_TAP_SIZE && touch.y >= screenH - SOUND_TAP_SIZE) {
    setSoundLevel((soundLevel + 1) % 3);
  } else if (touch.wasClicked() && gameOver && now - gameOverMs > 500) {
    startRun();
    beforeFirstRun = false;
    playSound(SND_START, FX_CHANNEL);
  }
  if (touch.wasHold()) {
    showNotice(setLevel() ? "level set" : "lay it flat to set level", now, 1500);
  }

  readTilt(dt);
  // Count how long the device has been left level during a run; any tilt
  // clears it.
  if (gameOver || pushX != 0 || pushY != 0) idleSec = 0;
  else idleSec += dt;

  // Physics runs in small fixed steps so bounces behave the same whatever
  // the frame rate. Everything freezes when the run is over.
  if (gameOver) {
    physicsDebt = 0;
  } else {
    physicsDebt += dt;
    while (physicsDebt >= PHYSICS_STEP && !gameOver) {
      stepPhysics(PHYSICS_STEP, now);
      physicsDebt -= PHYSICS_STEP;
    }
    trailX[trailHead] = puckX;
    trailY[trailHead] = puckY;
    trailHead = (trailHead + 1) % TRAIL_LENGTH;
  }
  puckFlash *= expf(-dt * 5.0f);
  // The moon keeps turning through its phases, across runs too.
  moonPhase += dt / MOON_CYCLE_SEC;
  if (moonPhase >= 1) moonPhase -= 1;
  starFlash *= expf(-dt * 4.0f);
  vanishLeft -= dt * 2.5f;
  if (vanishLeft < 0) vanishLeft = 0;

  updateSound();
  draw(now);

  fpsFrames++;
  if (now - fpsWindowStart >= 1000) {
    fps = fpsFrames * 1000 / (now - fpsWindowStart);
    fpsWindowStart = now;
    fpsFrames = 0;
  }
  // Only print when the serial buffer has room; with nothing listening a
  // print would block and stall the game.
  if (now - lastPrintMs >= 5000 && Serial.availableForWrite() >= 80) {
    lastPrintMs = now;
    Serial.printf("%d fps  tilt x %.2f y %.2f  stars %d  weights %d\n", fps, sensorX - levelX,
                  sensorY - levelY, score, weightCount);
  }
}
