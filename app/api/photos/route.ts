import { NextRequest, NextResponse } from "next/server";
import { list, put } from "@vercel/blob";
import {
  ALLOWED,
  GalleryResponse,
  LIMITS,
  PREFIX,
  Photo,
  buildPathname,
  cleanText,
  parsePathname,
  sniffMime,
} from "@/lib/photos";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

const configured = () => Boolean(process.env.BLOB_READ_WRITE_TOKEN);

/* --------------------------------------------------------------------------
 * Rate limiting
 * --------------------------------------------------------------------------
 * A sliding window held in module memory. Serverless instances recycle, so this
 * is a speed bump rather than a guarantee, but it is enough to stop a single
 * client hammering the upload route in a loop. The blob store itself is the
 * durable backstop.
 * ------------------------------------------------------------------------ */
const hits = new Map<string, number[]>();

function rateLimited(ip: string): boolean {
  const now = Date.now();
  const cutoff = now - LIMITS.windowMs;
  const recent = (hits.get(ip) ?? []).filter((t) => t > cutoff);
  recent.push(now);
  hits.set(ip, recent);

  // Keep the map from growing without bound across a long lived instance.
  if (hits.size > 5000) {
    for (const [key, times] of hits) {
      if (times.every((t) => t <= cutoff)) hits.delete(key);
    }
  }
  return recent.length > LIMITS.windowUploads;
}

function clientIp(req: NextRequest): string {
  const forwarded = req.headers.get("x-forwarded-for");
  return forwarded?.split(",")[0]?.trim() || req.headers.get("x-real-ip") || "unknown";
}

/* -------------------------------------------------------------------------- */

export async function GET(): Promise<NextResponse<GalleryResponse>> {
  if (!configured()) {
    return NextResponse.json({ configured: false, photos: [] });
  }

  try {
    const photos: Photo[] = [];
    let cursor: string | undefined;

    do {
      const page = await list({ prefix: PREFIX, limit: 1000, cursor });
      for (const blob of page.blobs) {
        const meta = parsePathname(blob.pathname);
        if (!meta.id) continue;
        photos.push({
          id: meta.id,
          url: blob.url,
          caption: meta.caption,
          uploader: meta.uploader,
          uploadedAt: new Date(blob.uploadedAt).toISOString(),
        });
      }
      cursor = page.hasMore ? page.cursor : undefined;
    } while (cursor);

    photos.sort((a, b) => b.uploadedAt.localeCompare(a.uploadedAt));
    return NextResponse.json({ configured: true, photos });
  } catch {
    return NextResponse.json({ configured: false, photos: [] });
  }
}

export async function POST(req: NextRequest) {
  if (!configured()) {
    return NextResponse.json(
      { error: "Photo storage is not connected yet. Add a Vercel Blob store to this project." },
      { status: 503 }
    );
  }

  if (rateLimited(clientIp(req))) {
    return NextResponse.json(
      { error: "That is a lot of photos very quickly. Take a breath and try again shortly." },
      { status: 429 }
    );
  }

  // Reject an oversized body before reading it into memory where we can.
  const declaredLength = Number(req.headers.get("content-length") ?? 0);
  if (declaredLength && declaredLength > LIMITS.maxBytes + 8 * 1024) {
    return NextResponse.json({ error: "That photo is larger than 12 MB." }, { status: 413 });
  }

  let form: FormData;
  try {
    form = await req.formData();
  } catch {
    return NextResponse.json({ error: "That upload was not readable." }, { status: 400 });
  }

  const file = form.get("file");
  if (!(file instanceof File)) {
    return NextResponse.json({ error: "No photo was attached." }, { status: 400 });
  }
  if (file.size === 0) {
    return NextResponse.json({ error: "That file is empty." }, { status: 400 });
  }
  if (file.size > LIMITS.maxBytes) {
    return NextResponse.json({ error: "That photo is larger than 12 MB." }, { status: 413 });
  }

  const buffer = Buffer.from(await file.arrayBuffer());

  // The browser supplied type is a hint, never the decision. The leading bytes
  // decide what this file actually is, and anything not on the allowlist stops
  // here. This is what keeps an SVG or an HTML payload from being stored and
  // then served back to a guest as if it were a photograph.
  const sniffed = sniffMime(new Uint8Array(buffer.subarray(0, 16)));
  if (!sniffed) {
    return NextResponse.json(
      { error: "Only JPEG, PNG, WebP and GIF images can be added." },
      { status: 415 }
    );
  }

  const id = crypto.randomUUID();
  const pathname = buildPathname({
    id,
    ext: ALLOWED[sniffed],
    caption: cleanText(form.get("caption"), LIMITS.maxCaption),
    uploader: cleanText(form.get("uploader"), LIMITS.maxUploader),
  });

  try {
    const blob = await put(pathname, buffer, {
      access: "public",
      contentType: sniffed,
      addRandomSuffix: false,
      cacheControlMaxAge: 60 * 60 * 24 * 365,
    });

    const meta = parsePathname(blob.pathname);
    const photo: Photo = {
      id,
      url: blob.url,
      caption: meta.caption,
      uploader: meta.uploader,
      uploadedAt: new Date().toISOString(),
    };
    return NextResponse.json({ photo }, { status: 201 });
  } catch {
    return NextResponse.json({ error: "Storing that photo failed. Please try again." }, { status: 500 });
  }
}
