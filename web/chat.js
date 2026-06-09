"use strict";

document.addEventListener(
    "DOMContentLoaded",
    async () => {
        const api =
            window.appApi;

        if (api === undefined) {
            throw new Error(
                "app.js должен быть подключён раньше chat.js"
            );
        }

        const form =
            document.getElementById("chat-form");

        const input =
            document.getElementById("chat-input");

        const messages =
            document.getElementById("chat-messages");

        const sendButton =
            document.getElementById("send-message");

        const stopButton =
            document.getElementById("stop-generating");

        const generatingMessage =
            document.getElementById("generating-message");

        const composerMenu =
            document.getElementById("composer-menu");

        const cheatsheetsModal =
            document.getElementById("cheatsheets-modal");

        if (
            form === null ||
            input === null ||
            messages === null ||
            sendButton === null ||
            stopButton === null ||
            generatingMessage === null ||
            composerMenu === null ||
            cheatsheetsModal === null
        ) {
            throw new Error(
                "На странице отсутствуют обязательные элементы чата"
            );
        }

        const serverStatusDot =
            document.querySelector(
                "[data-server-status-dot], #server-status-dot"
            );

        const serverStatusText =
            document.querySelector(
                "[data-server-status-text], #server-status-text"
            );

        const companyNameElement =
            document.querySelector(
                "[data-company-name], #company-name"
            );

        const subscriptionElement =
            document.querySelector(
                "[data-subscription], #subscription-plan"
            );

        let generationActive = false;
        let requestInFlight = false;
        let stopRequestInFlight = false;
        let previousServerGenerating = false;
        let statePollTimer = null;

        function scrollMessagesToBottom() {
            messages.scrollTop =
                messages.scrollHeight;
        }

        function resizeInput() {
            input.style.height = "auto";

            const maxHeight = 180;

            const nextHeight =
                Math.min(
                    input.scrollHeight,
                    maxHeight
                );

            input.style.height =
                `${nextHeight}px`;

            input.style.overflowY =
                input.scrollHeight > maxHeight
                    ? "auto"
                    : "hidden";
        }

        function appendInlineMarkdown(
            parent,
            text
        ) {
            const pattern =
                /(`[^`\n]+`|\*\*[^*\n]+?\*\*|__[^_\n]+?__|\*[^*\n]+?\*|_[^_\n]+?_)/g;

            let lastIndex = 0;
            let match;

            while (
                (match = pattern.exec(text)) !== null
                ) {
                if (match.index > lastIndex) {
                    parent.append(
                        document.createTextNode(
                            text.slice(
                                lastIndex,
                                match.index
                            )
                        )
                    );
                }

                const token =
                    match[0];

                if (
                    token.startsWith("**") &&
                    token.endsWith("**")
                ) {
                    const strong =
                        document.createElement("strong");

                    strong.textContent =
                        token.slice(2, -2);

                    parent.append(strong);
                } else if (
                    token.startsWith("__") &&
                    token.endsWith("__")
                ) {
                    const strong =
                        document.createElement("strong");

                    strong.textContent =
                        token.slice(2, -2);

                    parent.append(strong);
                } else if (
                    token.startsWith("`") &&
                    token.endsWith("`")
                ) {
                    const code =
                        document.createElement("code");

                    code.textContent =
                        token.slice(1, -1);

                    parent.append(code);
                } else {
                    const emphasis =
                        document.createElement("em");

                    emphasis.textContent =
                        token.slice(1, -1);

                    parent.append(emphasis);
                }

                lastIndex =
                    pattern.lastIndex;
            }

            if (lastIndex < text.length) {
                parent.append(
                    document.createTextNode(
                        text.slice(lastIndex)
                    )
                );
            }
        }

        function isBlockStart(line) {
            const trimmed =
                line.trim();

            return (
                trimmed.length === 0 ||
                /^#{1,6}\s+/.test(trimmed) ||
                /^[-*+]\s+/.test(trimmed) ||
                /^\d+[.)]\s+/.test(trimmed) ||
                /^>\s?/.test(trimmed) ||
                /^```/.test(trimmed) ||
                /^-{3,}$/.test(trimmed)
            );
        }

        function renderMarkdown(
            container,
            markdown
        ) {
            container.replaceChildren();

            const lines =
                String(markdown)
                    .replace(/\r\n?/g, "\n")
                    .split("\n");

            let index = 0;
            let activeList = null;
            let activeListTag = null;

            function closeList() {
                activeList = null;
                activeListTag = null;
            }

            function getList(tagName) {
                if (
                    activeList !== null &&
                    activeListTag === tagName
                ) {
                    return activeList;
                }

                closeList();

                activeList =
                    document.createElement(tagName);

                activeListTag =
                    tagName;

                container.append(activeList);

                return activeList;
            }

            while (index < lines.length) {
                const line =
                    lines[index];

                const trimmed =
                    line.trim();

                if (trimmed.length === 0) {
                    closeList();
                    ++index;
                    continue;
                }

                if (trimmed.startsWith("```")) {
                    closeList();

                    const language =
                        trimmed.slice(3).trim();

                    const codeLines = [];

                    ++index;

                    while (
                        index < lines.length &&
                        !lines[index]
                            .trim()
                            .startsWith("```")
                        ) {
                        codeLines.push(
                            lines[index]
                        );

                        ++index;
                    }

                    if (index < lines.length) {
                        ++index;
                    }

                    const pre =
                        document.createElement("pre");

                    const code =
                        document.createElement("code");

                    if (language.length !== 0) {
                        code.dataset.language =
                            language;
                    }

                    code.textContent =
                        codeLines.join("\n");

                    pre.append(code);
                    container.append(pre);

                    continue;
                }

                const headingMatch =
                    /^(#{1,6})\s+(.+)$/.exec(trimmed);

                if (headingMatch !== null) {
                    closeList();

                    const level =
                        headingMatch[1].length;

                    const heading =
                        document.createElement(
                            `h${level}`
                        );

                    appendInlineMarkdown(
                        heading,
                        headingMatch[2]
                    );

                    container.append(heading);

                    ++index;
                    continue;
                }

                const unorderedMatch =
                    /^[-*+]\s+(.+)$/.exec(trimmed);

                if (unorderedMatch !== null) {
                    const list =
                        getList("ul");

                    const item =
                        document.createElement("li");

                    appendInlineMarkdown(
                        item,
                        unorderedMatch[1]
                    );

                    list.append(item);

                    ++index;
                    continue;
                }

                const orderedMatch =
                    /^\d+[.)]\s+(.+)$/.exec(trimmed);

                if (orderedMatch !== null) {
                    const list =
                        getList("ol");

                    const item =
                        document.createElement("li");

                    appendInlineMarkdown(
                        item,
                        orderedMatch[1]
                    );

                    list.append(item);

                    ++index;
                    continue;
                }

                const quoteMatch =
                    /^>\s?(.*)$/.exec(trimmed);

                if (quoteMatch !== null) {
                    closeList();

                    const quote =
                        document.createElement(
                            "blockquote"
                        );

                    appendInlineMarkdown(
                        quote,
                        quoteMatch[1]
                    );

                    container.append(quote);

                    ++index;
                    continue;
                }

                if (/^-{3,}$/.test(trimmed)) {
                    closeList();

                    container.append(
                        document.createElement("hr")
                    );

                    ++index;
                    continue;
                }

                closeList();

                const paragraphLines = [
                    trimmed,
                ];

                ++index;

                while (
                    index < lines.length &&
                    !isBlockStart(lines[index])
                    ) {
                    paragraphLines.push(
                        lines[index].trim()
                    );

                    ++index;
                }

                const paragraph =
                    document.createElement("p");

                appendInlineMarkdown(
                    paragraph,
                    paragraphLines.join(" ")
                );

                container.append(paragraph);
            }
        }

        function createMessageElement({
                                          role,
                                          content,
                                          status = "completed",
                                      }) {
            const article =
                document.createElement("article");

            article.className =
                `message message--${role}`;

            article.dataset.status =
                status;

            const bubble =
                document.createElement("div");

            bubble.className =
                "message__bubble";

            const contentElement =
                document.createElement("div");

            if (role === "assistant") {
                contentElement.className =
                    "message__content markdown";

                renderMarkdown(
                    contentElement,
                    content
                );
            } else {
                contentElement.className =
                    "message__content message__content--plain";

                contentElement.textContent =
                    content;
            }

            bubble.append(contentElement);
            article.append(bubble);

            return article;
        }

        function appendUserMessage(
            content,
            status = "completed"
        ) {
            const message =
                createMessageElement({
                    role: "user",
                    content,
                    status,
                });

            messages.insertBefore(
                message,
                generatingMessage
            );

            scrollMessagesToBottom();

            return message;
        }

        function appendAssistantMessage(
            content,
            status = "completed"
        ) {
            const message =
                createMessageElement({
                    role: "assistant",
                    content,
                    status,
                });

            messages.insertBefore(
                message,
                generatingMessage
            );

            scrollMessagesToBottom();

            return message;
        }

        function clearRenderedMessages() {
            messages.replaceChildren();

            generatingMessage.hidden = true;

            messages.append(
                generatingMessage
            );
        }

        function renderHistory(historyMessages) {
            clearRenderedMessages();

            for (const message of historyMessages) {
                if (
                    typeof message?.content !== "string"
                ) {
                    continue;
                }

                if (message.role === "user") {
                    appendUserMessage(
                        message.content,
                        message.status
                    );

                    continue;
                }

                if (message.role === "assistant") {
                    appendAssistantMessage(
                        message.content,
                        message.status
                    );
                }
            }

            scrollMessagesToBottom();
        }

        function setGenerationState(
            active,
            {
                stopping = false,
            } = {}
        ) {
            generationActive = active;

            sendButton.hidden = active;
            stopButton.hidden = !active;

            stopButton.disabled =
                stopping || stopRequestInFlight;

            input.disabled = active;

            form.dataset.generating =
                active ? "true" : "false";

            form.dataset.stopping =
                stopping ? "true" : "false";

            if (active) {
                messages.append(
                    generatingMessage
                );

                generatingMessage.hidden = false;

                scrollMessagesToBottom();
                return;
            }

            generatingMessage.hidden = true;
            input.disabled = false;
            input.focus();
        }

        function completeGeneration(content) {
            appendAssistantMessage(content);
            setGenerationState(false);
        }

        function failGeneration(message) {
            appendAssistantMessage(
                message ??
                "Не удалось получить ответ. Попробуйте ещё раз.",
                "failed"
            );

            setGenerationState(false);
        }

        function closeComposerMenu() {
            composerMenu.open = false;
        }

        function openCheatsheets() {
            cheatsheetsModal.hidden = false;

            closeComposerMenu();

            cheatsheetsModal
                .querySelector(".modal__close")
                ?.focus();
        }

        function closeCheatsheets() {
            cheatsheetsModal.hidden = true;
            input.focus();
        }

        function clearInput() {
            input.value = "";
            resizeInput();
        }

        function handleApiRedirect(error) {
            const redirect =
                error?.payload?.redirect;

            if (
                typeof redirect === "string" &&
                redirect.length !== 0
            ) {
                window.location.assign(
                    redirect
                );

                return true;
            }

            return false;
        }

        async function loadHistory() {
            const payload =
                await api.getChatHistory();

            const historyMessages =
                Array.isArray(payload?.messages)
                    ? payload.messages
                    : [];

            renderHistory(
                historyMessages
            );
        }

        function updateApplicationState(state) {
            const serverRunning =
                state?.llama_server_running === true;

            const modelGenerating =
                state?.model_generates === true;

            document.documentElement.dataset.serverRunning =
                serverRunning ? "true" : "false";

            document.documentElement.dataset.modelGenerating =
                modelGenerating ? "true" : "false";

            if (serverStatusDot !== null) {
                serverStatusDot.dataset.status =
                    serverRunning
                        ? "online"
                        : "starting";

                serverStatusDot.title =
                    serverRunning
                        ? "Модель запущена"
                        : "Модель не запущена";
            }

            if (serverStatusText !== null) {
                serverStatusText.textContent =
                    serverRunning
                        ? "ИИ бот-помощник"
                        : "ИИ бот-помощник запускается";
            }

            if (
                companyNameElement !== null &&
                typeof state?.company_name === "string"
            ) {
                companyNameElement.textContent =
                    state.company_name;
            }

            if (
                subscriptionElement !== null &&
                typeof state?.subscription === "string"
            ) {
                subscriptionElement.textContent =
                    state.subscription;
            }

            if (
                modelGenerating &&
                !requestInFlight
            ) {
                setGenerationState(true);
            }

            if (
                previousServerGenerating &&
                !modelGenerating &&
                !requestInFlight
            ) {
                setGenerationState(false);

                loadHistory().catch(
                    (error) => {
                        console.error(
                            "Failed to reload history:",
                            error
                        );
                    }
                );
            }

            previousServerGenerating =
                modelGenerating;
        }

        async function refreshApplicationState() {
            try {
                const state =
                    await api.getApplicationState();

                updateApplicationState(state);
            } catch (error) {
                console.error(
                    "Failed to update application state:",
                    error
                );

                document.documentElement.dataset.serverRunning =
                    "false";

                if (serverStatusDot !== null) {
                    serverStatusDot.dataset.status =
                        "offline";

                    serverStatusDot.title =
                        "Локальный сервер недоступен";
                }

                if (serverStatusText !== null) {
                    serverStatusText.textContent =
                        "Локальный сервер недоступен";
                }
            }
        }

        async function submitMessage() {
            const message =
                input.value.trim();

            if (
                message.length === 0 ||
                generationActive ||
                requestInFlight
            ) {
                return;
            }

            appendUserMessage(
                message,
                "pending"
            );

            clearInput();

            requestInFlight = true;
            setGenerationState(true);

            try {
                const payload =
                    await api.sendChatMessage(
                        message
                    );

                const answer =
                    payload?.answer;

                if (
                    typeof answer !== "string" ||
                    answer.trim().length === 0
                ) {
                    throw new Error(
                        "Сервер вернул пустой ответ"
                    );
                }

                completeGeneration(answer);
            } catch (error) {
                if (handleApiRedirect(error)) {
                    return;
                }

                console.error(
                    "Chat request failed:",
                    error
                );

                failGeneration(
                    error?.message ??
                    "Не удалось получить ответ от бота."
                );
            } finally {
                requestInFlight = false;

                if (generationActive) {
                    setGenerationState(false);
                }

                refreshApplicationState();
            }
        }

        async function stopGeneration() {
            if (
                !generationActive ||
                stopRequestInFlight
            ) {
                return;
            }

            stopRequestInFlight = true;

            setGenerationState(
                true,
                {
                    stopping: true,
                }
            );

            try {
                await api.stopGeneration();
            } catch (error) {
                console.error(
                    "Failed to stop generation:",
                    error
                );

                failGeneration(
                    error?.message ??
                    "Не удалось остановить генерацию."
                );
            } finally {
                stopRequestInFlight = false;

                if (generationActive) {
                    setGenerationState(true);
                }
            }
        }

        async function clearHistory() {
            if (generationActive) {
                return;
            }

            const confirmed =
                window.confirm(
                    "Очистить всю историю чата?"
                );

            if (!confirmed) {
                return;
            }

            try {
                await api.clearChatHistory();

                clearRenderedMessages();
                closeComposerMenu();
                input.focus();
            } catch (error) {
                console.error(
                    "Failed to clear history:",
                    error
                );

                appendAssistantMessage(
                    error?.message ??
                    "Не удалось очистить историю.",
                    "failed"
                );
            }
        }

        form.addEventListener(
            "submit",
            (event) => {
                event.preventDefault();

                submitMessage();
            }
        );

        input.addEventListener(
            "input",
            resizeInput
        );

        input.addEventListener(
            "keydown",
            (event) => {
                if (
                    event.key !== "Enter" ||
                    event.isComposing
                ) {
                    return;
                }

                event.preventDefault();

                submitMessage();
            }
        );

        stopButton.addEventListener(
            "click",
            stopGeneration
        );

        document.addEventListener(
            "click",
            (event) => {
                if (
                    composerMenu.open &&
                    !composerMenu.contains(
                        event.target
                    )
                ) {
                    closeComposerMenu();
                }
            }
        );

        document
            .querySelector(
                '[data-action="open-cheatsheets"]'
            )
            ?.addEventListener(
                "click",
                openCheatsheets
            );

        document
            .querySelectorAll(
                '[data-action="close-cheatsheets"]'
            )
            .forEach((button) => {
                button.addEventListener(
                    "click",
                    closeCheatsheets
                );
            });

        document
            .querySelectorAll(
                "[data-cheatsheet-text]"
            )
            .forEach((button) => {
                button.addEventListener(
                    "click",
                    () => {
                        input.value =
                            button.textContent.trim();

                        resizeInput();
                        closeCheatsheets();
                        input.focus();
                    }
                );
            });

        document
            .querySelectorAll(
                '[data-action="clear-history"], ' +
                '[data-action="restart-chat"]'
            )
            .forEach((button) => {
                button.addEventListener(
                    "click",
                    clearHistory
                );
            });

        document.addEventListener(
            "keydown",
            (event) => {
                if (event.key !== "Escape") {
                    return;
                }

                closeComposerMenu();

                if (!cheatsheetsModal.hidden) {
                    closeCheatsheets();
                }
            }
        );

        resizeInput();
        setGenerationState(false);

        try {
            await loadHistory();
        } catch (error) {
            console.error(
                "Failed to load chat history:",
                error
            );

            clearRenderedMessages();

            appendAssistantMessage(
                "Не удалось загрузить историю чата. " +
                "Проверьте, что локальный сервер запущен.",
                "failed"
            );
        }

        await refreshApplicationState();

        statePollTimer =
            window.setInterval(
                refreshApplicationState,
                1500
            );

        window.addEventListener(
            "beforeunload",
            () => {
                if (statePollTimer !== null) {
                    window.clearInterval(
                        statePollTimer
                    );
                }
            }
        );

        window.chatPage =
            Object.freeze({
                loadHistory,
                refreshApplicationState,
                setGenerationState,
                appendUserMessage,
                appendAssistantMessage,
                completeGeneration,
                failGeneration,
            });
    }
);