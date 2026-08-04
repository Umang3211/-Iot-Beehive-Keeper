"use client";

import { useCallback, useEffect, useRef } from "react";
import type { Photo } from "@/lib/photos";

export default function Lightbox({
  photos,
  index,
  onClose,
  onNavigate,
}: {
  photos: Photo[];
  index: number;
  onClose: () => void;
  onNavigate: (next: number) => void;
}) {
  const closeRef = useRef<HTMLButtonElement>(null);
  const photo = photos[index];

  const go = useCallback(
    (step: number) => {
      if (photos.length === 0) return;
      onNavigate((index + step + photos.length) % photos.length);
    },
    [index, photos.length, onNavigate]
  );

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (event.key === "Escape") onClose();
      if (event.key === "ArrowRight") go(1);
      if (event.key === "ArrowLeft") go(-1);
    };
    document.addEventListener("keydown", onKey);

    // Hold the page still behind the overlay.
    const previous = document.body.style.overflow;
    document.body.style.overflow = "hidden";
    closeRef.current?.focus();

    return () => {
      document.removeEventListener("keydown", onKey);
      document.body.style.overflow = previous;
    };
  }, [go, onClose]);

  if (!photo) return null;

  return (
    <div
      role="dialog"
      aria-modal="true"
      aria-label={photo.caption || "Photo"}
      className="lightbox-veil fixed inset-0 z-50 flex items-center justify-center bg-ink-900/[0.93] p-4 backdrop-blur-md sm:p-8"
      onClick={onClose}
    >
      <button
        ref={closeRef}
        onClick={onClose}
        aria-label="Close"
        className="absolute right-4 top-4 z-10 rounded-full border border-white/15 bg-white/5 p-2.5 text-cream transition-colors hover:bg-white/15"
      >
        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
          <path d="M18 6 6 18M6 6l12 12" strokeLinecap="round" />
        </svg>
      </button>

      {photos.length > 1 && (
        <>
          <NavButton side="left" onClick={(e) => { e.stopPropagation(); go(-1); }} />
          <NavButton side="right" onClick={(e) => { e.stopPropagation(); go(1); }} />
        </>
      )}

      <figure
        className="lightbox-panel relative flex max-h-full w-full max-w-4xl flex-col items-center"
        onClick={(event) => event.stopPropagation()}
      >
        {/* eslint-disable-next-line @next/next/no-img-element */}
        <img
          key={photo.id}
          src={photo.url}
          alt={photo.caption || "A photo from the celebration"}
          className="max-h-[74vh] w-auto max-w-full rounded-lg object-contain shadow-2xl"
        />
        <figcaption className="mt-5 max-w-2xl px-2 text-center">
          {photo.caption && (
            <p className="font-hand text-2xl text-cream" style={{ fontFamily: "var(--font-hand), cursive" }}>
              {photo.caption}
            </p>
          )}
          <p className="mt-2 text-[0.7rem] uppercase tracking-[0.18em] text-muted">
            {photo.uploader ? `Added by ${photo.uploader}` : "Added by a guest"}
            <span className="mx-2 text-gold-400/60">/</span>
            {index + 1} of {photos.length}
          </p>
        </figcaption>
      </figure>
    </div>
  );
}

function NavButton({
  side,
  onClick,
}: {
  side: "left" | "right";
  onClick: (event: React.MouseEvent) => void;
}) {
  return (
    <button
      onClick={onClick}
      aria-label={side === "left" ? "Previous photo" : "Next photo"}
      className={`absolute top-1/2 z-10 -translate-y-1/2 rounded-full border border-white/15 bg-white/5 p-3 text-cream transition-all hover:border-gold-400/60 hover:bg-white/15 ${
        side === "left" ? "left-3 sm:left-6" : "right-3 sm:right-6"
      }`}
    >
      <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
        <path
          d={side === "left" ? "M15 18l-6-6 6-6" : "M9 18l6-6-6-6"}
          strokeLinecap="round"
          strokeLinejoin="round"
        />
      </svg>
    </button>
  );
}
