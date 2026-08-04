import type { Metadata, Viewport } from "next";
import { Caveat, Fraunces, Manrope } from "next/font/google";
import { person } from "@/lib/content";
import "./globals.css";

const display = Fraunces({
  subsets: ["latin"],
  variable: "--font-display",
  display: "swap",
  axes: ["SOFT", "WONK", "opsz"],
});

const body = Manrope({
  subsets: ["latin"],
  variable: "--font-body",
  display: "swap",
});

const hand = Caveat({
  subsets: ["latin"],
  variable: "--font-hand",
  display: "swap",
  weight: ["400", "600"],
});

export const metadata: Metadata = {
  title: `${person.name} is ${person.milestone}`,
  description: `A photo wall, a life story, and a place to leave a memory for ${person.name}'s ${person.milestone}th birthday.`,
  robots: { index: false, follow: false },
  openGraph: {
    title: `${person.name} is ${person.milestone}`,
    description: `Come and add a photo to the wall for ${person.shortName}.`,
    type: "website",
  },
};

export const viewport: Viewport = {
  themeColor: "#0d0a14",
};

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en" className={`${display.variable} ${body.variable} ${hand.variable}`}>
      <body>{children}</body>
    </html>
  );
}
