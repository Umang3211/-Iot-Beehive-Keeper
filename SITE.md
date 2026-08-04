# Abhishek at 50

A birthday site: his story, a wall of photographs everyone can add to, and a chat
that answers in his voice.

Built with Next.js. It lives on the `claude/abhishek-50th-birthday-site-f8aioo`
branch alongside the beehive firmware already in this repository.

---

## Getting it live on Vercel

### 1. Import the repository

Go to https://vercel.com/new and import `Umang3211/-Iot-Beehive-Keeper`.
Leave every build setting at its default, Vercel detects Next.js on its own.

### 2. Point Vercel at the right branch

The site is not on `main`. Straight after importing, open
**Settings → Git → Production Branch**, change it to:

```
claude/abhishek-50th-birthday-site-f8aioo
```

Then **Deployments → Redeploy**. Skipping this step means Vercel builds `main`,
which has no website in it, and the build fails.

### 3. Connect photo storage

Photos need somewhere to live. In the project, open **Storage → Create Database
→ Blob**, create the store and connect it. Vercel adds the
`BLOB_READ_WRITE_TOKEN` environment variable automatically.

Redeploy once more and uploading works.

Until this step is done the site runs fine, the wall simply shows an empty state
and the upload panel explains that storage is not connected yet.

### 4. Load the photos from the video

Open the site, go to **Add photos**, and drag the whole `check` folder in. Select
them all at once, the uploader queues them and sends them one at a time. Large
photos are shrunk in the browser first, so a folder of full resolution originals
is fine.

---

## Making it about the real Abhishek

Everything the site says lives in one file: **`lib/content.ts`**.

| What to change | Where |
| --- | --- |
| Name, birth year, party date, hero line | `person` |
| The four playful stats under the hero | `factCards` |
| The biography paragraphs | `biography` |
| The scrolling timeline | `timeline` |
| What the chat says, and what triggers it | `intents` |
| The opening line and suggested questions | `chatGreeting`, `chatSuggestions` |

Anything still generic is marked with a `TODO` comment. Edit, commit, push, and
Vercel redeploys on its own.

The chat matches on keywords, so more keywords per intent means better answers.
Each intent holds several replies and picks between them at random, so asking
twice does not give the same line twice.

---

## How uploads are kept safe

Guest uploads are open by design, so the checks sit on the server where a guest
cannot skip them.

- **Real file type, not the claimed one.** Every upload is identified by its
  leading bytes. A file that claims `image/jpeg` but is actually an SVG or an
  HTML document is rejected. Only JPEG, PNG, WebP and GIF are stored. SVG is
  excluded on purpose, it can carry script.
- **Size.** 12 MB per photo, checked before the body is read where possible.
- **Rate limiting.** 30 uploads per IP per 10 minutes.
- **Filenames are generated, never taken from the user.** Each photo is stored
  under a fresh UUID with the extension forced to match the sniffed type, so no
  guest controls a path.
- **Captions and names are encoded, clamped and stripped of control
  characters,** then escaped by React on the way out.
- **Content Security Policy** on every response, with `object-src 'none'` and
  `frame-ancestors 'none'`. Photos are served from Vercel's blob domain, a
  different origin to the site.
- **The site asks search engines not to index it.** It is unlisted, not secret.
  Anyone with the link can view and upload.

If the link spreads further than you would like, the quickest fix is Vercel's
**Deployment Protection**, or ask and I will add a shared passcode and an
approval queue so nothing appears until you approve it.

---

## Running it locally

```bash
npm install
npm run dev
```

Then open http://localhost:3000. Without `BLOB_READ_WRITE_TOKEN` set, uploads
report that storage is not connected. Everything else works.
