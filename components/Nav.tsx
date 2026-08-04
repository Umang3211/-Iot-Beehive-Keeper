"use client";

import { useEffect, useState } from "react";
import { person } from "@/lib/content";

const LINKS = [
  { href: "#story", label: "The story" },
  { href: "#wall", label: "Photo wall" },
  { href: "#add", label: "Add photos" },
  { href: "#chat", label: "Chat" },
];

export default function Nav() {
  const [solid, setSolid] = useState(false);

  useEffect(() => {
    const onScroll = () => setSolid(window.scrollY > 80);
    onScroll();
    window.addEventListener("scroll", onScroll, { passive: true });
    return () => window.removeEventListener("scroll", onScroll);
  }, []);

  return (
    <nav
      className={`fixed inset-x-0 top-0 z-40 transition-all duration-300 ${
        solid ? "border-b border-white/10 bg-ink-900/85 backdrop-blur-lg" : "border-b border-transparent"
      }`}
    >
      <div className="shell flex h-16 items-center justify-between">
        <a href="#top" className="font-display text-lg font-semibold tracking-tight text-cream">
          {person.shortName}
          <span className="text-gold-400"> @ {person.milestone}</span>
        </a>

        <div className="hidden items-center gap-7 sm:flex">
          {LINKS.map((link) => (
            <a
              key={link.href}
              href={link.href}
              className="text-sm text-muted transition-colors hover:text-gold-400"
            >
              {link.label}
            </a>
          ))}
        </div>

        <a href="#add" className="btn-gold px-4 py-2 text-xs sm:hidden">
          Add photos
        </a>
      </div>
    </nav>
  );
}
