import Biography from "@/components/Biography";
import Chat from "@/components/Chat";
import Hero from "@/components/Hero";
import Nav from "@/components/Nav";
import PhotoWall from "@/components/PhotoWall";
import { person } from "@/lib/content";

export default function Page() {
  return (
    <>
      <Nav />
      <main id="top">
        <Hero />
        <Biography />
        <PhotoWall />
        <Chat />
      </main>

      <footer className="relative border-t border-white/10 py-14">
        <div className="shell text-center">
          <p className="font-display text-2xl text-cream">
            Happy {person.milestone}th, {person.shortName}
          </p>
          <p className="prose-warm mx-auto mt-3 max-w-md text-balance text-sm">
            Made by the people who have been putting up with the jokes the longest.
          </p>
          <p className="mt-6 text-[0.7rem] uppercase tracking-[0.2em] text-muted/60">
            {person.name} &middot; {person.birthYear} to {person.birthYear + person.milestone}
          </p>
        </div>
      </footer>
    </>
  );
}
