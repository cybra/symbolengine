// plugin.js: a plugin editor's parameters, for its page. It is served to every editor, after symbolengine.js.

(() => {
    if (window.plugin)
        return;

    class Parameter {
        constructor(about) {
            Object.assign(this, about);
            this.handlers = new Set();
        }

        // Calls the handler with the parameter whenever its value changes: from the page, the host or a preset.
        // Gives a function that stops it.
        onChange(handler) {
            this.handlers.add(handler);
            return () => this.handlers.delete(handler);
        }

        // The start and end of a gesture, such as a drag, around the values set during it, so a host records them
        // as one stretch of automation.
        begin() {
            return symbolengine.call("__begin", this.id);
        }

        async set(value) {
            const now = await symbolengine.call("__set", this.id, Number(value));
            this.changed(now.value, now.text);
            return this.value;
        }

        end() {
            return symbolengine.call("__end", this.id);
        }

        // A change made all at once, such as a click or a typed value.
        async change(value) {
            await this.begin();
            try {
                return await this.set(value);
            } finally {
                await this.end();
            }
        }

        // The text the parameter shows for a value, such as "-6.0 dB".
        textFor(value) {
            return symbolengine.call("__text", this.id, Number(value));
        }

        // The value as a share of its range, from 0 to 1, and back, for knobs and sliders.
        get share() {
            return this.maximum > this.minimum ? (this.value - this.minimum) / (this.maximum - this.minimum) : 0;
        }

        valueAt(share) {
            const value = this.minimum + Math.min(Math.max(share, 0), 1) * (this.maximum - this.minimum);
            return this.stepped ? Math.round(value) : value;
        }

        changed(value, text) {
            if (value === this.value && text === this.text)
                return;
            this.value = value;
            this.text = text;
            for (const handler of this.handlers) {
                try {
                    handler(this);
                } catch (error) {
                    console.error(error);
                }
            }
        }
    }

    let loaded = null;

    const load = async () => {
        const parameters = new Map();
        for (const about of await symbolengine.call("__parameters"))
            parameters.set(about.id, new Parameter(about));

        symbolengine.on("__parameter", ({ id, value, text }) => parameters.get(id)?.changed(value, text));
        return parameters;
    };

    // A resizable editor gets a grip in its bottom right corner, as hosts such as Live don't let a plugin's window be
    // resized from its edges. A page restyles it, or hides it, through the class symbolengine-grip. While it is
    // dragged, one size is asked for at a time, always the latest, so the host isn't flooded.
    const addGrip = async () => {
        const editor = await symbolengine.call("__editor");
        if (!editor.resizable)
            return;

        const grip = document.createElement("div");
        grip.className = "symbolengine-grip";
        grip.innerHTML = `<svg viewBox="0 0 14 14" width="14" height="14" aria-hidden="true">
            <path d="M13 5 L5 13 M13 9 L9 13" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/></svg>`;
        grip.style.cssText = "position:fixed; right:1px; bottom:1px; width:16px; height:16px; display:flex;" +
                             "align-items:center; justify-content:center; cursor:nwse-resize; opacity:0.5;" +
                             "z-index:2147483647; touch-action:none; user-select:none; -webkit-user-select:none";

        let start = null, wanted = null, asking = false;

        const ask = async () => {
            if (asking || wanted === null)
                return;
            asking = true;
            const size = wanted;
            wanted = null;
            try {
                await symbolengine.call("__resize", size.width, size.height);
            } finally {
                asking = false;
                ask();
            }
        };

        grip.addEventListener("pointerdown", async event => {
            event.preventDefault();
            try {
                grip.setPointerCapture(event.pointerId);
            } catch {
            }
            const from = { x: event.screenX, y: event.screenY };
            const now = await symbolengine.call("__editor");
            start = { x: from.x, y: from.y, width: now.width, height: now.height };
        });

        grip.addEventListener("pointermove", event => {
            if (start === null)
                return;
            wanted = { width: Math.round(start.width + event.screenX - start.x),
                       height: Math.round(start.height + event.screenY - start.y) };
            ask();
        });

        const stop = () => {
            start = null;
        };
        grip.addEventListener("pointerup", stop);
        grip.addEventListener("pointercancel", stop);

        document.body.append(grip);
    };

    if (document.readyState === "loading")
        document.addEventListener("DOMContentLoaded", addGrip);
    else
        addGrip();

    window.plugin = Object.freeze({
        // The plugin's parameters, as a Map from each one's id.
        parameters: () => (loaded ??= load()),

        // Asks the host to resize a resizable editor, in points, held to its limits. Gives the size it has after, and
        // whether the host agreed.
        resize: (width, height) => symbolengine.call("__resize", Number(width), Number(height)),
    });
})();
