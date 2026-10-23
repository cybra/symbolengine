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

    window.plugin = Object.freeze({
        // The plugin's parameters, as a Map from each one's id.
        parameters: () => (loaded ??= load()),

        // Asks the host to resize a resizable editor, in points, held to its limits. Gives the size it has after, and
        // whether the host agreed.
        resize: (width, height) => symbolengine.call("__resize", Number(width), Number(height)),
    });
})();
