// =============================================================================
// PhotoBooth cloud gallery — Cloudflare Worker.
//
// The booth uploads each session's photos here; guests open the QR link on
// their own mobile data (no booth WiFi). See README.md for setup.
//
// Bindings required (Worker > Settings):
//   * KV namespace binding:  PHOTOS   (create a KV namespace, bind it)
//   * Environment variable:  UPLOAD_KEY  (same value as CLOUD_UPLOAD_KEY in
//     shared/photobooth/photobooth_config.h)
//
// Routes:
//   PUT /up/<sess>/<n>.jpg?k=KEY   store a photo (from the booth)
//   PUT /up/<sess>/meta?k=KEY      store session metadata JSON
//   PUT /up/f/<name>.png?k=KEY     store a filter overlay PNG
//   GET /s/<sess>                  gallery page
//   GET /p/<sess>/<n>.jpg          photo bytes
//   GET /f/<name>.png              filter overlay bytes
// =============================================================================

const SESS_RE = /^[A-Z0-9]{3,8}$/;
const FLT_RE = /^[A-Za-z0-9._-]{5,31}\.png$/i;

function html(body, extraHead = "") {
  return new Response(
    `<!doctype html><html><head><meta charset=utf-8>` +
      `<meta name=viewport content='width=device-width,initial-scale=1'>` +
      `<title>PhotoBooth</title>${extraHead}<style>` +
      `body{margin:0;background:#111;color:#eee;font-family:system-ui,sans-serif;text-align:center}` +
      `h1{padding:16px;font-weight:600}` +
      `.grid{display:grid;gap:12px;padding:12px;grid-template-columns:repeat(auto-fit,minmax(240px,1fr))}` +
      `.photo{background:#fff;padding:10px 10px 40px;border-radius:6px;box-shadow:0 4px 20px rgba(0,0,0,.5)}` +
      `.photo img{width:100%;display:block;border-radius:2px}` +
      `</style></head><body>${body}</body></html>`,
    { headers: { "content-type": "text/html;charset=utf-8" } },
  );
}

// Same client-side compositing as the booth's local gallery: crop every
// photo to the band the booth screen showed, draw the filter overlay 1:1.
const COMPOSE_JS = `<script>
document.querySelectorAll('.photo img').forEach(function(im){
  var ph=new Image(),fl=null,n=0,need=1;
  var fu=im.getAttribute('data-flt');
  if(fu){fl=new Image();need=2;}
  function go(){if(++n<need)return;
    var c=document.createElement('canvas');
    var cw=ph.naturalWidth,chh=cw*480/800;
    c.width=cw;c.height=chh;
    var x=c.getContext('2d');
    x.drawImage(ph,0,(ph.naturalHeight-chh)/2,cw,chh,0,0,cw,chh);
    if(fl){var s=cw/800;
      var fx=(800-fl.naturalWidth)/2,fy=(480-fl.naturalHeight)/2;
      x.drawImage(fl,fx*s,fy*s,fl.naturalWidth*s,fl.naturalHeight*s);}
    im.src=c.toDataURL('image/jpeg',0.92);}
  ph.onload=go;ph.src=im.src;
  if(fl){fl.onload=go;fl.src=fu;}});
</script>`;

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    const p = url.pathname;

    // ---- Uploads from the booth ------------------------------------------
    if (req.method === "PUT" && p.startsWith("/up/")) {
      if (url.searchParams.get("k") !== env.UPLOAD_KEY)
        return new Response("forbidden", { status: 403 });
      const key = p.slice(4); // "<sess>/<n>.jpg" | "<sess>/meta" | "f/<name>"
      const okPhoto = /^[A-Z0-9]{3,8}\/([1-8]\.jpg|meta)$/.test(key);
      const okFlt = key.startsWith("f/") && FLT_RE.test(key.slice(2));
      if (!okPhoto && !okFlt) return new Response("bad key", { status: 400 });
      const body = await req.arrayBuffer();
      if (body.byteLength < 1 || body.byteLength > 1024 * 1024)
        return new Response("bad size", { status: 400 });
      // Photos auto-expire after 30 days; filters are tiny, keep them.
      await env.PHOTOS.put(key, body, okFlt ? {} : { expirationTtl: 2592000 });
      return new Response("ok");
    }

    // ---- Gallery page ------------------------------------------------------
    if (p.startsWith("/s/")) {
      const sess = p.slice(3);
      if (!SESS_RE.test(sess)) return new Response("bad id", { status: 400 });
      const meta = await env.PHOTOS.get(`${sess}/meta`, "json");
      if (!meta || !meta.n) {
        return html(
          `<h1>Almost there...</h1><p>Your photos are still uploading.<br>` +
            `This page refreshes automatically.</p>`,
          `<meta http-equiv=refresh content=3>`,
        );
      }
      const f = meta.f || {};
      let imgs = "";
      for (let i = 1; i <= meta.n; i++) {
        const flt = f[i] ? ` data-flt="/f/${f[i]}"` : "";
        imgs += `<div class=photo><img src="/p/${sess}/${i}.jpg"${flt}></div>`;
      }
      return html(
        `<h1>Your photos</h1><div class=grid>${imgs}</div>` +
          `<p>Tap and hold a photo to save it.</p>${COMPOSE_JS}`,
      );
    }

    // ---- Photo / filter bytes ---------------------------------------------
    if (p.startsWith("/p/") || p.startsWith("/f/")) {
      const key = p.startsWith("/p/") ? p.slice(3) : "f/" + p.slice(3);
      const data = await env.PHOTOS.get(key, "arrayBuffer");
      if (!data) return new Response("not found", { status: 404 });
      const type = key.endsWith(".png") ? "image/png" : "image/jpeg";
      return new Response(data, {
        headers: {
          "content-type": type,
          "cache-control": "public, max-age=86400",
        },
      });
    }

    return html(`<h1>PhotoBooth</h1><p>Scan a QR at the booth!</p>`);
  },
};
