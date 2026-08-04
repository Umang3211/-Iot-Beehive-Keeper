/**
 * Shared photo types plus the encoding used to keep guest captions alongside the
 * image in blob storage.
 *
 * Metadata is packed into the blob pathname rather than a separate manifest file
 * on purpose. A party means many people uploading at the same moment, and a
 * single shared manifest would need a read, a modify and a write per upload,
 * which loses photos whenever two guests overlap. A pathname is written once by
 * the upload that owns it, so concurrent uploads cannot clobber each other.
 */

export type Photo = {
  id: string;
  url: string;
  caption: string;
  uploader: string;
  uploadedAt: string;
};

export type GalleryResponse = {
  configured: boolean;
  photos: Photo[];
};

export const LIMITS = {
  maxBytes: 12 * 1024 * 1024,
  maxCaption: 140,
  maxUploader: 40,
  /** Uploads allowed per IP inside the rate limit window. */
  windowUploads: 30,
  windowMs: 10 * 60 * 1000,
} as const;

/** Only formats every browser renders as an image. SVG is deliberately absent. */
export const ALLOWED = {
  "image/jpeg": "jpg",
  "image/png": "png",
  "image/webp": "webp",
  "image/gif": "gif",
} as const;

export type AllowedMime = keyof typeof ALLOWED;

/**
 * Identify a file by its leading bytes rather than trusting the browser supplied
 * content type, which any client can set to whatever it likes. A file that
 * claims to be a JPEG but does not start like one never reaches storage.
 */
export function sniffMime(bytes: Uint8Array): AllowedMime | null {
  const at = (i: number) => bytes[i];

  if (bytes.length >= 3 && at(0) === 0xff && at(1) === 0xd8 && at(2) === 0xff) {
    return "image/jpeg";
  }
  if (
    bytes.length >= 8 &&
    at(0) === 0x89 &&
    at(1) === 0x50 &&
    at(2) === 0x4e &&
    at(3) === 0x47 &&
    at(4) === 0x0d &&
    at(5) === 0x0a &&
    at(6) === 0x1a &&
    at(7) === 0x0a
  ) {
    return "image/png";
  }
  if (
    bytes.length >= 6 &&
    at(0) === 0x47 &&
    at(1) === 0x49 &&
    at(2) === 0x46 &&
    at(3) === 0x38 &&
    (at(4) === 0x37 || at(4) === 0x39) &&
    at(5) === 0x61
  ) {
    return "image/gif";
  }
  // RIFF....WEBP
  if (
    bytes.length >= 12 &&
    at(0) === 0x52 &&
    at(1) === 0x49 &&
    at(2) === 0x46 &&
    at(3) === 0x46 &&
    at(8) === 0x57 &&
    at(9) === 0x45 &&
    at(10) === 0x42 &&
    at(11) === 0x50
  ) {
    return "image/webp";
  }
  return null;
}

/** Strip control characters and clamp. Applied on the way in and on the way out. */
const CONTROL_CHARS = new RegExp("[\\u0000-\\u001f\\u007f-\\u009f]", "g");

export function cleanText(value: unknown, max: number): string {
  if (typeof value !== "string") return "";
  return value.replace(CONTROL_CHARS, " ").replace(/\s+/g, " ").trim().slice(0, max);
}

const b64url = {
  encode: (value: string) => Buffer.from(value, "utf8").toString("base64url"),
  decode: (value: string) => {
    try {
      return Buffer.from(value, "base64url").toString("utf8");
    } catch {
      return "";
    }
  },
};

export const PREFIX = "photos/";

/**
 * Field separator. It must be a character that base64url can never emit,
 * otherwise an encoded caption could contain the separator and split into the
 * wrong fields. The base64url alphabet is A-Z a-z 0-9 plus "-" and "_", so a
 * hyphen is unsafe here: the caption "௾" encodes to "4K--". A tilde is outside
 * the alphabet and is unreserved in URLs, so it survives the round trip.
 */
const SEP = "~";

export function buildPathname(opts: {
  id: string;
  ext: string;
  caption: string;
  uploader: string;
}): string {
  const caption = b64url.encode(cleanText(opts.caption, LIMITS.maxCaption));
  const uploader = b64url.encode(cleanText(opts.uploader, LIMITS.maxUploader));
  return `${PREFIX}${opts.id}${SEP}${uploader}${SEP}${caption}.${opts.ext}`;
}

export function parsePathname(pathname: string): { id: string; caption: string; uploader: string } {
  const file = pathname.slice(PREFIX.length).replace(/\.[a-z0-9]+$/i, "");
  const [id = "", uploader = "", caption = ""] = file.split(SEP);
  return {
    id,
    uploader: cleanText(b64url.decode(uploader), LIMITS.maxUploader),
    caption: cleanText(b64url.decode(caption), LIMITS.maxCaption),
  };
}
