// Persists the user's chosen recordings folder across restarts. Stored next to
// llm-config.json in userData rather than inside the recordings folder itself,
// since the whole point is that folder can move.
import { readFileSync } from "node:fs";
import fs from "node:fs/promises";
import path from "node:path";

interface RecordingsLocationConfig {
	recordingsDir: string | null;
}

export class RecordingsLocationStore {
	private readonly configPath: string;
	private config: RecordingsLocationConfig = { recordingsDir: null };

	constructor(userDataPath: string) {
		this.configPath = path.join(userDataPath, "recordings-location.json");
		this.loadSync();
	}

	private loadSync(): void {
		try {
			const raw = readFileSync(this.configPath, "utf8");
			const parsed = JSON.parse(raw);
			this.config = {
				recordingsDir: typeof parsed.recordingsDir === "string" ? parsed.recordingsDir : null,
			};
		} catch {
			this.config = { recordingsDir: null };
		}
	}

	/** The user's custom folder, or null to use the default (userData/recordings). */
	getCustomDir(): string | null {
		return this.config.recordingsDir;
	}

	async setCustomDir(dir: string | null): Promise<void> {
		this.config = { recordingsDir: dir };
		await fs.writeFile(this.configPath, JSON.stringify(this.config, null, 2), "utf8");
	}
}
