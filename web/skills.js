// Skill registry and loader — mirrors core/skill_loader.cpp. Scans the
// chat's `.gab/skills/` folder for subdirectories containing a `SKILL.md`
// with YAML-style front matter (`---\nname: foo\ndescription: bar\n---`).
// Skills are declared in `{{SKILLS}}` in the system prompt; their content
// is lazily loaded and injected into the conversation by the `skill(name)`
// tool call.

function trim(s) { return String(s).replace(/^\s+|\s+$/g, ''); }

function parseFrontMatter(content) {
    // Accept leading whitespace then ---\n ... \n---
    const m = content.match(/^\s*---\s*\n([\s\S]*?)\n---/);
    if (!m) return null;

    const body = m[1];
    const fm = { name: '', description: '' };
    for (const rawLine of body.split('\n')) {
        const line = trim(rawLine);
        if (!line || line.startsWith('#')) continue;
        const colon = line.indexOf(':');
        if (colon < 0) continue;
        const key = trim(line.slice(0, colon));
        let val = trim(line.slice(colon + 1));
        if (val.length >= 2 && val.startsWith('"') && val.endsWith('"')) {
            val = val.slice(1, -1);
        }
        if (key === 'name') fm.name = val;
        else if (key === 'description') fm.description = val;
    }
    if (!fm.name) return null;
    return fm;
}

export class SkillLoader {
    constructor(vfs) {
        this.vfs = vfs;
        this.registry = new Map();  // name → {description, dirPath}
        this.loaded = new Map();    // name → loaded content (injected into convo)
    }

    async scan() {
        this.registry.clear();
        const entries = await this.vfs.listDir('.gab/skills');
        for (const entry of entries) {
            if (!entry.isDir) continue;
            const skillDir = `.gab/skills/${entry.name}`;
            const files = await this.vfs.listDir(skillDir);
            let skillMdPath = null;
            for (const f of files) {
                if (f.isDir) continue;
                if (f.name.toLowerCase() === 'skill.md') {
                    skillMdPath = `${skillDir}/${f.name}`;
                    break;
                }
            }
            if (!skillMdPath) continue;
            const content = await this.vfs.readFile(skillMdPath);
            if (!content) continue;
            const fm = parseFrontMatter(content);
            if (!fm) continue;
            this.registry.set(fm.name, {
                description: fm.description,
                dirPath: skillDir,
            });
        }
    }

    // Concatenate SKILL.md (first) with any other .md files in the skill dir.
    async load(name) {
        const meta = this.registry.get(name);
        if (!meta) return '';
        const files = await this.vfs.listDir(meta.dirPath);
        const mdPaths = [];
        let skillMd = null;
        for (const f of files) {
            if (f.isDir) continue;
            const lower = f.name.toLowerCase();
            if (!lower.endsWith('.md')) continue;
            const path = `${meta.dirPath}/${f.name}`;
            if (lower === 'skill.md') skillMd = path;
            else mdPaths.push(path);
        }
        mdPaths.sort();
        if (skillMd) mdPaths.unshift(skillMd);
        let result = '';
        for (const p of mdPaths) {
            const c = await this.vfs.readFile(p);
            if (c) result += c + '\n\n';
        }
        return result;
    }

    exists(name) { return this.registry.has(name); }

    generateSummaries() {
        if (this.registry.size === 0) return '(no skills available)\n';
        let out = '';
        for (const [name, meta] of this.registry) {
            out += `- **${name}**: ${meta.description}\n`;
        }
        return out;
    }

    markLoaded(name, content) { this.loaded.set(name, content); }
    isLoaded(name) { return this.loaded.has(name); }
    clearLoaded() { this.loaded.clear(); }
}
