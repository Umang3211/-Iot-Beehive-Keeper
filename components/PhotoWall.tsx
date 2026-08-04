"use client";

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import Lightbox from "@/components/Lightbox";
import Reveal from "@/components/Reveal";
import { ALLOWED, LIMITS, type GalleryResponse, type Photo } from "@/lib/photos";
import { person } from "@/lib/content";

type QueueItem = {
  key: string;
  name: string;
  preview: string;
  progress: number;
  state: "waiting" | "sending" | "done" | "failed";
  error?: string;
};

type Confetti = { key: number; left: number; dx: string; spin: string; dur: string; color: string };

const ACCEPT = Object.keys(ALLOWED).join(",");
const CONFETTI_COLORS = ["#f5c451", "#e2703a", "#f6efe2", "#c08a1c", "#8d6fd1"];

/** Stable pseudo random tilt per photo so prints do not re-angle on every render. */
function tiltFor(id: string): string {
  let hash = 0;
  for (let i = 0; i < id.length; i++) hash = (hash * 31 + id.charCodeAt(i)) | 0;
  const degrees = ((Math.abs(hash) % 500) / 100 - 2.5).toFixed(2);
  return `${degrees}deg`;
}

/**
 * Shrink very large photos in the browser before sending. A modern phone photo
 * can easily exceed the server limit, and a guest at a party should not have to
 * think about that. Animated GIFs are left alone because a canvas would flatten
 * them to a single frame.
 */
async function prepareFile(file: File): Promise<File> {
  const TOO_BIG = 2.5 * 1024 * 1024;
  const MAX_EDGE = 2400;

  if (file.size <= TOO_BIG || file.type === "image/gif") return file;
  if (typeof createImageBitmap === "undefined") return file;

  try {
    const bitmap = await createImageBitmap(file);
    const scale = Math.min(1, MAX_EDGE / Math.max(bitmap.width, bitmap.height));
    if (scale === 1 && file.size <= LIMITS.maxBytes) {
      bitmap.close();
      return file;
    }

    const canvas = document.createElement("canvas");
    canvas.width = Math.round(bitmap.width * scale);
    canvas.height = Math.round(bitmap.height * scale);

    const context = canvas.getContext("2d");
    if (!context) {
      bitmap.close();
      return file;
    }
    context.drawImage(bitmap, 0, 0, canvas.width, canvas.height);
    bitmap.close();

    const blob = await new Promise<Blob | null>((resolve) =>
      canvas.toBlob(resolve, "image/jpeg", 0.86)
    );
    if (!blob || blob.size >= file.size) return file;

    return new File([blob], file.name.replace(/\.[^.]+$/, "") + ".jpg", { type: "image/jpeg" });
  } catch {
    return file;
  }
}

function uploadOne(
  file: File,
  fields: { caption: string; uploader: string },
  onProgress: (percent: number) => void
): Promise<Photo> {
  return new Promise((resolve, reject) => {
    const form = new FormData();
    form.append("file", file);
    form.append("caption", fields.caption);
    form.append("uploader", fields.uploader);

    // XHR rather than fetch, purely because it reports upload progress and a
    // large photo on party wifi needs a progress bar to feel alive.
    const xhr = new XMLHttpRequest();
    xhr.open("POST", "/api/photos");
    xhr.upload.addEventListener("progress", (event) => {
      if (event.lengthComputable) onProgress(Math.round((event.loaded / event.total) * 100));
    });
    xhr.addEventListener("load", () => {
      let payload: { photo?: Photo; error?: string } = {};
      try {
        payload = JSON.parse(xhr.responseText);
      } catch {
        /* handled below */
      }
      if (xhr.status >= 200 && xhr.status < 300 && payload.photo) resolve(payload.photo);
      else reject(new Error(payload.error || "That upload did not go through."));
    });
    xhr.addEventListener("error", () => reject(new Error("The connection dropped mid upload.")));
    xhr.send(form);
  });
}

export default function PhotoWall() {
  const [photos, setPhotos] = useState<Photo[]>([]);
  const [status, setStatus] = useState<"loading" | "ready" | "unconfigured">("loading");
  const [fresh, setFresh] = useState<Set<string>>(new Set());
  const [view, setView] = useState<"wall" | "reel">("wall");
  const [lightbox, setLightbox] = useState<number | null>(null);

  const [queue, setQueue] = useState<QueueItem[]>([]);
  const [dragging, setDragging] = useState(false);
  const [uploader, setUploader] = useState("");
  const [caption, setCaption] = useState("");
  const [notice, setNotice] = useState<string | null>(null);
  const [confetti, setConfetti] = useState<Confetti[]>([]);

  const inputRef = useRef<HTMLInputElement>(null);
  const busy = queue.some((item) => item.state === "waiting" || item.state === "sending");

  useEffect(() => {
    let cancelled = false;
    fetch("/api/photos")
      .then((response) => response.json() as Promise<GalleryResponse>)
      .then((data) => {
        if (cancelled) return;
        setPhotos(data.photos);
        setStatus(data.configured ? "ready" : "unconfigured");
      })
      .catch(() => !cancelled && setStatus("ready"));
    return () => {
      cancelled = true;
    };
  }, []);

  // Remember who someone said they were, so a guest adding photos across the
  // evening only types their name once.
  useEffect(() => {
    try {
      const saved = localStorage.getItem("guest-name");
      if (saved) setUploader(saved);
    } catch {
      /* private browsing, no harm */
    }
  }, []);

  const fireConfetti = useCallback(() => {
    if (window.matchMedia("(prefers-reduced-motion: reduce)").matches) return;
    const pieces: Confetti[] = Array.from({ length: 46 }, (_, i) => ({
      key: Date.now() + i,
      left: Math.random() * 100,
      dx: `${(Math.random() - 0.5) * 340}px`,
      spin: `${540 + Math.random() * 720}deg`,
      dur: `${2.4 + Math.random() * 1.9}s`,
      color: CONFETTI_COLORS[i % CONFETTI_COLORS.length],
    }));
    setConfetti(pieces);
    window.setTimeout(() => setConfetti([]), 4600);
  }, []);

  const handleFiles = useCallback(
    async (fileList: FileList | File[]) => {
      const incoming = Array.from(fileList);
      if (incoming.length === 0) return;

      setNotice(null);
      try {
        localStorage.setItem("guest-name", uploader);
      } catch {
        /* ignore */
      }

      const accepted: { item: QueueItem; file: File }[] = [];
      const rejected: string[] = [];

      for (const file of incoming) {
        if (!(file.type in ALLOWED)) {
          rejected.push(`${file.name} is not a JPEG, PNG, WebP or GIF`);
          continue;
        }
        accepted.push({
          item: {
            key: `${file.name}-${file.size}-${Math.random().toString(36).slice(2)}`,
            name: file.name,
            preview: URL.createObjectURL(file),
            progress: 0,
            state: "waiting",
          },
          file,
        });
      }

      if (rejected.length) setNotice(rejected.join(". "));
      if (accepted.length === 0) return;

      setQueue((current) => [...current, ...accepted.map((entry) => entry.item)]);

      const patch = (key: string, changes: Partial<QueueItem>) =>
        setQueue((current) =>
          current.map((item) => (item.key === key ? { ...item, ...changes } : item))
        );

      let landed = 0;

      // Sequential on purpose. Twenty photos fired at once on venue wifi is how
      // uploads start timing out.
      for (const { item, file } of accepted) {
        patch(item.key, { state: "sending" });
        try {
          const prepared = await prepareFile(file);
          if (prepared.size > LIMITS.maxBytes) {
            throw new Error("Still larger than 12 MB after resizing.");
          }
          const photo = await uploadOne(prepared, { caption, uploader }, (percent) =>
            patch(item.key, { progress: percent })
          );

          setPhotos((current) => [photo, ...current]);
          setFresh((current) => new Set(current).add(photo.id));
          patch(item.key, { state: "done", progress: 100 });
          landed++;
        } catch (error) {
          patch(item.key, {
            state: "failed",
            error: error instanceof Error ? error.message : "Upload failed.",
          });
        }
      }

      if (landed > 0) {
        fireConfetti();
        setCaption("");
      }

      // Clear the finished rows shortly after, keeping any failures on screen.
      window.setTimeout(() => {
        setQueue((current) => {
          current.filter((i) => i.state === "done").forEach((i) => URL.revokeObjectURL(i.preview));
          return current.filter((item) => item.state !== "done");
        });
      }, 2600);
    },
    [caption, uploader, fireConfetti]
  );

  const onDrop = useCallback(
    (event: React.DragEvent) => {
      event.preventDefault();
      setDragging(false);
      if (event.dataTransfer?.files?.length) handleFiles(event.dataTransfer.files);
    },
    [handleFiles]
  );

  const counter = useMemo(() => {
    if (status === "loading") return "Loading the wall";
    if (photos.length === 0) return "The wall is empty, be the first";
    return `${photos.length} ${photos.length === 1 ? "photo" : "photos"} so far`;
  }, [photos.length, status]);

  return (
    <>
      {/* Confetti lives outside the section so it falls across the whole page. */}
      {confetti.map((piece) => (
        <span
          key={piece.key}
          aria-hidden
          className="confetti-piece"
          style={
            {
              left: `${piece.left}%`,
              background: piece.color,
              "--dx": piece.dx,
              "--spin": piece.spin,
              "--dur": piece.dur,
            } as React.CSSProperties
          }
        />
      ))}

      {/* ---------------------------------------------------------------- */}
      {/* Upload                                                            */}
      {/* ---------------------------------------------------------------- */}
      <section id="add" className="relative py-24 sm:py-28">
        <div className="shell">
          <Reveal className="text-center">
            <p className="eyebrow">Your turn</p>
            <h2 className="section-title mt-3 text-balance">Put a photo on the wall</h2>
            <p className="prose-warm mx-auto mt-4 max-w-lg text-balance">
              Anything you have of {person.shortName}. The blurry ones count double.
            </p>
          </Reveal>

          <Reveal delay={90} className="mx-auto mt-10 max-w-2xl">
            <div className="card-glass p-5 sm:p-7">
              <div className="grid gap-3 sm:grid-cols-2">
                <label className="block">
                  <span className="mb-1.5 block text-[0.7rem] uppercase tracking-[0.16em] text-muted">
                    Your name
                  </span>
                  <input
                    className="field"
                    value={uploader}
                    maxLength={LIMITS.maxUploader}
                    onChange={(event) => setUploader(event.target.value)}
                    placeholder="Who is this from?"
                  />
                </label>
                <label className="block">
                  <span className="mb-1.5 block text-[0.7rem] uppercase tracking-[0.16em] text-muted">
                    Caption <span className="normal-case tracking-normal">(optional)</span>
                  </span>
                  <input
                    className="field"
                    value={caption}
                    maxLength={LIMITS.maxCaption}
                    onChange={(event) => setCaption(event.target.value)}
                    placeholder="Goa, 2004, nobody remembers why"
                  />
                </label>
              </div>

              <div
                className={`dropzone mt-4 rounded-2xl border-2 border-dashed border-white/15 p-8 text-center ${
                  dragging ? "dropzone--over" : ""
                }`}
                onDragOver={(event) => {
                  event.preventDefault();
                  setDragging(true);
                }}
                onDragLeave={() => setDragging(false)}
                onDrop={onDrop}
              >
                <svg
                  aria-hidden
                  className="mx-auto mb-3 text-gold-400"
                  width="38"
                  height="38"
                  viewBox="0 0 24 24"
                  fill="none"
                  stroke="currentColor"
                  strokeWidth="1.5"
                >
                  <path d="M12 16V4m0 0L7.5 8.5M12 4l4.5 4.5" strokeLinecap="round" strokeLinejoin="round" />
                  <path d="M3 15v3a3 3 0 0 0 3 3h12a3 3 0 0 0 3-3v-3" strokeLinecap="round" />
                </svg>

                <p className="font-body text-sm text-cream">
                  Drag photos here, or{" "}
                  <button
                    type="button"
                    onClick={() => inputRef.current?.click()}
                    className="font-bold text-gold-400 underline underline-offset-4 hover:text-gold-500"
                  >
                    choose from your device
                  </button>
                </p>
                <p className="mt-1.5 text-xs text-muted">
                  JPEG, PNG, WebP or GIF, up to 12 MB each. Select as many as you like.
                </p>

                <input
                  ref={inputRef}
                  type="file"
                  accept={ACCEPT}
                  multiple
                  className="sr-only"
                  onChange={(event) => {
                    if (event.target.files) handleFiles(event.target.files);
                    event.target.value = "";
                  }}
                />
              </div>

              {notice && (
                <p role="alert" className="mt-3 rounded-lg bg-ember/15 px-3 py-2 text-sm text-ember">
                  {notice}
                </p>
              )}

              {status === "unconfigured" && (
                <p className="mt-3 rounded-lg border border-gold-400/30 bg-gold-400/10 px-3 py-2 text-sm text-gold-400">
                  Photo storage is not connected yet. Add a Blob store to the Vercel project and
                  redeploy, then uploads will work.
                </p>
              )}

              {queue.length > 0 && (
                <ul className="mt-4 space-y-2">
                  {queue.map((item) => (
                    <li key={item.key} className="flex items-center gap-3 rounded-xl bg-white/[0.04] p-2.5">
                      {/* eslint-disable-next-line @next/next/no-img-element */}
                      <img
                        src={item.preview}
                        alt=""
                        className="h-11 w-11 flex-none rounded-lg object-cover"
                      />
                      <div className="min-w-0 flex-1">
                        <p className="truncate text-xs text-cream">{item.name}</p>
                        {item.state === "failed" ? (
                          <p className="text-xs text-ember">{item.error}</p>
                        ) : (
                          <div className="mt-1.5 h-1 overflow-hidden rounded-full bg-white/10">
                            <div
                              className="h-full rounded-full bg-gold-400 transition-[width] duration-200"
                              style={{ width: `${item.state === "done" ? 100 : item.progress}%` }}
                            />
                          </div>
                        )}
                      </div>
                      <span className="flex-none text-xs text-muted">
                        {item.state === "done" ? "Added" : item.state === "failed" ? "Failed" : `${item.progress}%`}
                      </span>
                    </li>
                  ))}
                </ul>
              )}

              <p className="mt-4 text-center text-xs text-muted">
                {busy ? "Uploading, keep this tab open" : "Photos appear on the wall straight away"}
              </p>
            </div>
          </Reveal>
        </div>
      </section>

      {/* ---------------------------------------------------------------- */}
      {/* The wall                                                          */}
      {/* ---------------------------------------------------------------- */}
      <section id="wall" className="relative scroll-mt-20 pb-28">
        <div className="shell">
          <Reveal className="mb-9 flex flex-wrap items-end justify-between gap-4">
            <div>
              <p className="eyebrow">The photo wall</p>
              <h2 className="section-title mt-3">Fifty years, all at once</h2>
              <p className="mt-3 text-sm text-muted">{counter}</p>
            </div>

            {photos.length > 0 && (
              <div className="flex gap-1 rounded-full border border-white/[0.12] bg-white/[0.04] p-1">
                {(["wall", "reel"] as const).map((mode) => (
                  <button
                    key={mode}
                    onClick={() => setView(mode)}
                    aria-pressed={view === mode}
                    className={`rounded-full px-4 py-1.5 text-xs font-bold uppercase tracking-[0.12em] transition-colors ${
                      view === mode ? "bg-gold-400 text-ink-900" : "text-muted hover:text-cream"
                    }`}
                  >
                    {mode}
                  </button>
                ))}
              </div>
            )}
          </Reveal>

          {status === "loading" && (
            <div className="photo-wall">
              {[0, 1, 2, 3, 4, 5].map((i) => (
                <div
                  key={i}
                  className="mb-6 animate-pulse rounded bg-white/[0.05]"
                  style={{ height: `${210 + (i % 3) * 90}px`, breakInside: "avoid" }}
                />
              ))}
            </div>
          )}

          {status !== "loading" && photos.length === 0 && (
            <div className="card-glass px-6 py-20 text-center">
              <p className="font-display text-2xl text-cream">Nothing here yet</p>
              <p className="prose-warm mx-auto mt-2 max-w-sm">
                The first photo is always the hardest. Scroll up and start the wall off.
              </p>
              <a href="#add" className="btn-gold mt-6">
                Add the first photo
              </a>
            </div>
          )}

          {photos.length > 0 && view === "wall" && (
            <div className="photo-wall">
              {photos.map((photo, i) => (
                <figure
                  key={photo.id}
                  role="button"
                  tabIndex={0}
                  aria-label={photo.caption || "Open photo"}
                  onClick={() => setLightbox(i)}
                  onKeyDown={(event) => {
                    if (event.key === "Enter" || event.key === " ") {
                      event.preventDefault();
                      setLightbox(i);
                    }
                  }}
                  className={`polaroid relative ${i % 3 === 1 ? "polaroid--taped" : ""} ${
                    fresh.has(photo.id) ? "polaroid--new" : ""
                  }`}
                  style={{ ["--tilt" as string]: tiltFor(photo.id) }}
                >
                  {fresh.has(photo.id) && <span className="badge-new">Just added</span>}
                  <div className="polaroid__frame">
                    {/* eslint-disable-next-line @next/next/no-img-element */}
                    <img
                      src={photo.url}
                      alt={photo.caption || "A photo from the celebration"}
                      loading="lazy"
                      decoding="async"
                      className="polaroid__img"
                    />
                  </div>
                  <figcaption className="polaroid__caption">
                    {photo.caption || " "}
                    {photo.uploader && <span className="polaroid__by mt-1 block">{photo.uploader}</span>}
                  </figcaption>
                </figure>
              ))}
            </div>
          )}

          {photos.length > 0 && view === "reel" && (
            <div className="thin-scroll -mx-5 flex snap-x snap-mandatory gap-5 overflow-x-auto px-5 pb-6 sm:-mx-8 sm:px-8">
              {photos.map((photo, i) => (
                <button
                  key={photo.id}
                  onClick={() => setLightbox(i)}
                  className="group relative w-[78vw] flex-none snap-center overflow-hidden rounded-2xl border border-white/10 sm:w-[440px]"
                >
                  {/* eslint-disable-next-line @next/next/no-img-element */}
                  <img
                    src={photo.url}
                    alt={photo.caption || "A photo from the celebration"}
                    loading="lazy"
                    decoding="async"
                    className="h-[58vh] w-full object-cover transition-transform duration-500 group-hover:scale-[1.04]"
                  />
                  <div className="absolute inset-x-0 bottom-0 bg-gradient-to-t from-ink-900 via-ink-900/70 to-transparent p-5 text-left">
                    <p
                      className="text-xl text-cream"
                      style={{ fontFamily: "var(--font-hand), cursive" }}
                    >
                      {photo.caption || "No caption, just vibes"}
                    </p>
                    {photo.uploader && (
                      <p className="mt-0.5 text-[0.65rem] uppercase tracking-[0.16em] text-muted">
                        {photo.uploader}
                      </p>
                    )}
                  </div>
                </button>
              ))}
            </div>
          )}
        </div>
      </section>

      {lightbox !== null && (
        <Lightbox
          photos={photos}
          index={lightbox}
          onClose={() => setLightbox(null)}
          onNavigate={setLightbox}
        />
      )}
    </>
  );
}
