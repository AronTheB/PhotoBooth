# PhotoBooth cloud gallery — 5-minute setup

With this, guests never join the booth's WiFi: the booth uploads each session
over YOUR internet (phone hotspot is fine), and the QR is a normal `https://`
link they open on their own mobile data.

## 1. Create the Worker (one time)

1. Make a free account at https://dash.cloudflare.com (no credit card).
2. Left menu: **Workers & Pages** → **Create** → **Create Worker** →
   name it e.g. `photobooth` → **Deploy**.
3. Click **Edit code**, delete the sample, paste ALL of `worker.js`, **Deploy**.

## 2. Add the two bindings

1. Worker page → **Settings** → **Variables and Secrets** → **Add**:
   - Name: `UPLOAD_KEY`, value: any random string you invent
     (e.g. `banana-rocket-42`). Remember it.
2. Left menu: **Storage & Databases** → **KV** → **Create namespace** →
   name it `photobooth-photos`.
3. Back on the Worker: **Settings** → **Bindings** → **Add** →
   **KV namespace** → Variable name: `PHOTOS`, select `photobooth-photos`.
4. **Deploy** again if prompted.

## 3. Point the booth at it

Edit `shared/photobooth/photobooth_config.h`:

```c
#define STA_SSID "IPhone"
#define STA_PASSWORD "aronwifi"
#define CLOUD_BASE_URL "https://photobooth.arontheb.workers.dev"  // no trailing /
#define CLOUD_UPLOAD_KEY "random_key_67"   // the same UPLOAD_KEY value
```

(The worker URL is shown on the Worker's overview page.)

Reflash the main node. Done.

## How it behaves

- Booth online: QR = `https://.../s/A1B2`. Guests open it on mobile data.
  If they scan before the upload finishes, the page says "still uploading"
  and refreshes itself.
- Booth offline (hotspot missing): everything falls back to the old local
  WiFi gallery automatically — nothing breaks.
- Photos auto-delete from the cloud after 30 days. The SD card keeps the
  originals forever either way.

## Free-tier limits (Cloudflare KV)

- 1000 uploads/day — that's ~300-500 photos/day with metadata. Plenty for a
  party; if you ever host a festival, tell Claude and we move it to R2.
