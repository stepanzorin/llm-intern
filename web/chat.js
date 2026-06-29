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

        const welcomeMessages = Object.freeze({
            workflow: "Какой у Вас рабочий вопрос?",
            texting: "Пришлите сообщение клиента или опишите ситуацию — подготовлю ответ.",
        });

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

        const assistantProfileMenu =
            document.getElementById("assistant-profile-menu");

        const assistantProfileLabel =
            document.getElementById("assistant-profile-label");

        const assistantProfileButtons =
            document.querySelectorAll("[data-assistant-profile]");

        const cheatsheetProfileGrids =
            document.querySelectorAll("[data-cheatsheets-profile]");

        const cheatsheetsModal =
            document.getElementById("cheatsheets-modal");

        const restartServerButton =
            document.querySelector('[data-action="restart-server"]');

        if (
            form === null ||
            input === null ||
            messages === null ||
            sendButton === null ||
            stopButton === null ||
            generatingMessage === null ||
            composerMenu === null ||
            assistantProfileMenu === null ||
            assistantProfileLabel === null ||
            assistantProfileButtons.length === 0 ||
            cheatsheetsModal === null
        ) {
            throw new Error(
                "На странице отсутствуют обязательные элементы чата"
            );
        }

        const serverStatusDots =
            document.querySelectorAll(
                "[data-server-status-dot], #server-status-dot, #llama-server-status-dot"
            );

        const serverStatusMainTexts =
            document.querySelectorAll(
                "[data-server-status-text]"
            );

        const serverStatusDetailTexts =
            document.querySelectorAll(
                "#llama-server-status-text"
            );

        const companyNameElements =
            document.querySelectorAll(
                "[data-company-name], #company-name"
            );

        const subscriptionElements =
            document.querySelectorAll(
                "[data-subscription], #subscription-plan, #subscription-name"
            );

        let generationActive = false;
        let requestInFlight = false;
        let stopRequestInFlight = false;
        let restartRequestInFlight = false;
        let generationCancelledByUser = false;
        let previousServerGenerating = false;
        let statePollTimer = null;
        let activeAssistantProfile = "workflow";
        let historyLoaded = false;
        let profileChangeInFlight = false;
        let profileState = new Map();

        function setTextForAll(nodes, text) {
            nodes.forEach((node) => {
                node.textContent = text;
            });
        }

        function profileLabel(profile) {
            return profile === "texting"
                ? "Ответы клиентам"
                : "Рабочие инструкции";
        }

        function profileAvailable(profile) {
            return profileState.get(profile)?.available !== false;
        }

        function closeAssistantProfileMenu() {
            assistantProfileMenu.open = false;
        }

        function updateProfileControlsDisabled() {
            assistantProfileButtons.forEach((button) => {
                const profile = button.dataset.assistantProfile;
                const available = profileAvailable(profile);

                button.dataset.available = available ? "true" : "false";
                button.disabled =
                    generationActive ||
                    requestInFlight ||
                    restartRequestInFlight ||
                    profileChangeInFlight ||
                    !available;

                if (!available) {
                    button.title = "Доступно только на тарифе PREMIUM";
                } else {
                    button.removeAttribute("title");
                }
            });

            if (
                generationActive ||
                requestInFlight ||
                restartRequestInFlight ||
                profileChangeInFlight
            ) {
                closeAssistantProfileMenu();
            }
        }

        function applyAssistantProfile(profile) {
            const normalizedProfile =
                profile === "texting"
                    ? "texting"
                    : "workflow";

            const changed =
                activeAssistantProfile !== normalizedProfile;

            activeAssistantProfile = normalizedProfile;
            document.documentElement.dataset.assistantProfile =
                normalizedProfile;
            assistantProfileLabel.textContent =
                profileLabel(normalizedProfile);

            assistantProfileButtons.forEach((button) => {
                button.setAttribute(
                    "aria-pressed",
                    button.dataset.assistantProfile === normalizedProfile
                        ? "true"
                        : "false"
                );
            });

            cheatsheetProfileGrids.forEach((grid) => {
                grid.hidden =
                    grid.dataset.cheatsheetsProfile !== normalizedProfile;
            });

            input.placeholder =
                normalizedProfile === "texting"
                    ? "Вставьте сообщение клиента или опишите ситуацию..."
                    : "Напишите рабочий вопрос...";

            updateProfileControlsDisabled();

            return changed;
        }

        async function parseJsonResponse(response) {
            let payload = {};

            try {
                payload = await response.json();
            } catch {
                payload = {};
            }

            if (!response.ok) {
                const error = new Error(
                    payload?.error?.message ??
                    "Не удалось выполнить запрос."
                );

                error.payload = payload;
                throw error;
            }

            return payload;
        }

        async function changeAssistantProfile(profile) {
            if (
                profile === activeAssistantProfile ||
                !profileAvailable(profile) ||
                generationActive ||
                requestInFlight ||
                restartRequestInFlight ||
                profileChangeInFlight
            ) {
                closeAssistantProfileMenu();
                return;
            }

            profileChangeInFlight = true;
            updateProfileControlsDisabled();

            try {
                const response = await fetch(
                    "/api/chat/profile",
                    {
                        method: "PUT",
                        headers: {
                            "Content-Type": "application/json",
                        },
                        body: JSON.stringify({
                            profile,
                        }),
                    }
                );

                const payload =
                    await parseJsonResponse(response);

                applyAssistantProfile(
                    payload?.assistant_profile
                );

                clearRenderedMessages();
                await loadHistory();
                await refreshApplicationState();
                focusInputWithoutScroll();
            } catch (error) {
                if (handleApiRedirect(error)) {
                    return;
                }

                console.error(
                    "Failed to change assistant profile:",
                    error
                );

                appendAssistantMessage(
                    error?.message ??
                    "Не удалось переключить режим бота.",
                    "failed"
                );
            } finally {
                profileChangeInFlight = false;
                closeAssistantProfileMenu();
                updateProfileControlsDisabled();
            }
        }

        function scrollMessagesToBottom() {
            messages.scrollTop =
                messages.scrollHeight;
        }

        function focusInputWithoutScroll() {
            try {
                input.focus({
                    preventScroll: true,
                });
            } catch {
                input.focus();
            }
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

        function appendInlineMarkdown(parent, text) {
            const pattern =
                /(`[^`\n]+`|\*\*[^*\n]+?\*\*|__[^_\n]+?__|\*[^*\n]+?\*|_[^_\n]+?_)/g;

            let lastIndex = 0;
            let match;

            while ((match = pattern.exec(text)) !== null) {
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

        function getMarkdownIndent(indent) {
            return indent.replace(/\t/g, "    ").length;
        }

        function isBlockStart(line) {
            const trimmed =
                line.trim();

            return (
                trimmed.length === 0 ||
                /^#{1,6}\s+/.test(trimmed) ||
                /^\s*[-*+]\s+/.test(line) ||
                /^\s*\d+[.)]\s+/.test(line) ||
                /^>\s?/.test(trimmed) ||
                /^```/.test(trimmed) ||
                /^-{3,}$/.test(trimmed)
            );
        }

        function renderMarkdown(container, markdown) {
            container.replaceChildren();

            const lines =
                String(markdown)
                    .replace(/\r\n?/g, "\n")
                    .split("\n");

            let index = 0;

            const listStack = [];

            function closeLists() {
                listStack.length = 0;
            }

            function currentListState() {
                return listStack.length === 0
                    ? null
                    : listStack[listStack.length - 1];
            }

            function appendListToParent(list, indent) {
                const parentState =
                    currentListState();

                if (
                    parentState !== null &&
                    indent > parentState.indent &&
                    parentState.lastItem !== null
                ) {
                    parentState.lastItem.append(list);
                    return;
                }

                container.append(list);
            }

            function ensureList({
                                    indent,
                                    tagName,
                                    startNumber = 1,
                                }) {
                while (
                    currentListState() !== null &&
                    indent < currentListState().indent
                ) {
                    listStack.pop();
                }

                if (
                    currentListState() !== null &&
                    indent === currentListState().indent &&
                    tagName !== currentListState().tagName
                ) {
                    listStack.pop();
                }

                if (
                    currentListState() !== null &&
                    indent === currentListState().indent &&
                    tagName === currentListState().tagName
                ) {
                    return currentListState();
                }

                const list =
                    document.createElement(tagName);

                if (
                    tagName === "ol" &&
                    Number.isInteger(startNumber) &&
                    startNumber > 1
                ) {
                    list.start = startNumber;
                }

                appendListToParent(list, indent);

                const state = {
                    indent,
                    tagName,
                    list,
                    lastItem: null,
                };

                listStack.push(state);

                return state;
            }

            function appendListItem({
                                        indent,
                                        tagName,
                                        text,
                                        startNumber = 1,
                                    }) {
                const state =
                    ensureList({
                        indent,
                        tagName,
                        startNumber,
                    });

                const item =
                    document.createElement("li");

                appendInlineMarkdown(
                    item,
                    text
                );

                state.list.append(item);
                state.lastItem = item;
            }

            while (index < lines.length) {
                const line =
                    lines[index];

                const trimmed =
                    line.trim();

                if (trimmed.length === 0) {
                    ++index;
                    continue;
                }

                if (trimmed.startsWith("```")) {
                    closeLists();

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
                    closeLists();

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
                    /^(\s*)[-*+]\s+(.+)$/.exec(line);

                if (unorderedMatch !== null) {
                    appendListItem({
                        indent: getMarkdownIndent(
                            unorderedMatch[1]
                        ),
                        tagName: "ul",
                        text: unorderedMatch[2],
                    });

                    ++index;
                    continue;
                }

                const orderedMatch =
                    /^(\s*)(\d+)[.)]\s+(.+)$/.exec(line);

                if (orderedMatch !== null) {
                    appendListItem({
                        indent: getMarkdownIndent(
                            orderedMatch[1]
                        ),
                        tagName: "ol",
                        text: orderedMatch[3],
                        startNumber: Number(
                            orderedMatch[2]
                        ),
                    });

                    ++index;
                    continue;
                }

                const quoteMatch =
                    /^\s*>\s?(.*)$/.exec(line);

                if (quoteMatch !== null) {
                    closeLists();

                    const quoteLines = [];

                    while (index < lines.length) {
                        const nestedQuoteMatch =
                            /^\s*>\s?(.*)$/.exec(
                                lines[index]
                            );

                        if (nestedQuoteMatch === null) {
                            break;
                        }

                        quoteLines.push(
                            nestedQuoteMatch[1]
                        );

                        ++index;
                    }

                    const quote =
                        document.createElement(
                            "blockquote"
                        );

                    renderMarkdown(
                        quote,
                        quoteLines.join("\n")
                    );

                    container.append(quote);

                    continue;
                }

                if (/^-{3,}$/.test(trimmed)) {
                    closeLists();

                    container.append(
                        document.createElement("hr")
                    );

                    ++index;
                    continue;
                }

                closeLists();

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
                                          kind = "history",
                                      }) {
            const article =
                document.createElement("article");

            article.className =
                `message message--${role}`;

            article.dataset.status =
                status;

            article.dataset.kind =
                kind;

            const bubble =
                document.createElement("div");

            bubble.className =
                "message__bubble";

            const contentElement =
                document.createElement("div");

            if (
                role === "assistant" &&
                activeAssistantProfile !== "texting"
            ) {
                contentElement.className =
                    "message__content markdown";

                renderMarkdown(
                    contentElement,
                    content
                );
            } else {
                contentElement.className =
                    "message__content message__content--plain";

                if (role === "assistant") {
                    contentElement.classList.add(
                        "message__content--client-reply"
                    );
                }

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

        function appendWelcomeMessage() {
            const existingWelcomeMessage =
                messages.querySelector(
                    '[data-kind="welcome"]'
                );

            if (existingWelcomeMessage !== null) {
                return;
            }

            appendAssistantMessage(
                welcomeMessages[activeAssistantProfile],
                "completed",
                {
                    kind: "welcome",
                }
            );
        }

        function removeWelcomeMessage() {
            messages
                .querySelectorAll('[data-kind="welcome"]')
                .forEach((messageElement) => {
                    messageElement.remove();
                });
        }

        function appendAssistantMessage(
            content,
            status = "completed",
            {
                kind = "history",
            } = {}
        ) {
            const message =
                createMessageElement({
                    role: "assistant",
                    content,
                    status,
                    kind,
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

            const realMessages =
                historyMessages.filter((message) => {
                    return (
                        typeof message?.content === "string" &&
                        (
                            message.role === "user" ||
                            message.role === "assistant"
                        )
                    );
                });

            if (realMessages.length === 0) {
                appendWelcomeMessage();
                scrollMessagesToBottom();
                return;
            }

            for (const message of realMessages) {
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
                scroll = true,
                focus = true,
            } = {}
        ) {
            generationActive = active;
            updateProfileControlsDisabled();

            sendButton.hidden = active;
            stopButton.hidden = !active;

            /*
             * Важно:
             * Не используем stopButton.disabled для состояния "останавливаю".
             * disabled на активной/focused кнопке может провоцировать скачок скролла.
             *
             * Повторные клики всё равно блокируются через stopRequestInFlight.
             */
            stopButton.disabled = false;

            stopButton.dataset.stopping =
                stopping ? "true" : "false";

            stopButton.setAttribute(
                "aria-disabled",
                stopping ? "true" : "false"
            );

            input.disabled = active;

            form.dataset.generating =
                active ? "true" : "false";

            form.dataset.stopping =
                stopping ? "true" : "false";

            messages.setAttribute(
                "aria-busy",
                active ? "true" : "false"
            );

            if (active) {
                /*
                 * Добавляем индикатор только если он реально не внизу.
                 * При нажатии "Стоп" эту функцию больше не вызываем.
                 */
                if (
                    generatingMessage.parentElement !== messages ||
                    messages.lastElementChild !== generatingMessage
                ) {
                    messages.append(generatingMessage);
                }

                generatingMessage.hidden = false;

                if (scroll) {
                    scrollMessagesToBottom();
                }

                return;
            }

            generatingMessage.hidden = true;
            input.disabled = false;

            stopButton.dataset.stopping = "false";
            stopButton.setAttribute("aria-disabled", "false");

            if (focus) {
                focusInputWithoutScroll();
            }
        }

        function completeGeneration(content) {
            appendAssistantMessage(content);

            setGenerationState(
                false,
                {
                    focus: true,
                }
            );
        }

        function failGeneration(message) {
            appendAssistantMessage(
                message ??
                "Не удалось получить ответ. Попробуйте ещё раз.",
                "failed"
            );

            setGenerationState(
                false,
                {
                    focus: true,
                }
            );
        }

        function setServerVisualState({
                                          running,
                                          generating = false,
                                          restarting = false,
                                          offline = false,
                                      }) {
            let dotStatus;
            let dotTitle;
            let mainText;
            let detailText;

            if (offline) {
                dotStatus = "offline";
                dotTitle = "Локальный сервер недоступен";
                mainText = "Локальный сервер недоступен";
                detailText = "Нет соединения";
            } else if (restarting) {
                dotStatus = "starting";
                dotTitle = "Модель перезапускается";
                mainText = "ИИ бот-помощник перезапускается";
                detailText = "Модель перезапускается";
            } else if (running && generating) {
                dotStatus = "online";
                dotTitle = "Модель формирует ответ";
                mainText = "ИИ бот-помощник отвечает";
                detailText = "Формирует ответ";
            } else if (running) {
                dotStatus = "online";
                dotTitle = "Модель запущена";
                mainText = "ИИ бот-помощник";
                detailText = "Модель запущена";
            } else {
                dotStatus = "starting";
                dotTitle = "Модель запускается";
                mainText = "ИИ бот-помощник запускается";
                detailText = "Модель запускается";
            }

            document.documentElement.dataset.serverRunning =
                running ? "true" : "false";

            document.documentElement.dataset.modelGenerating =
                generating ? "true" : "false";

            serverStatusDots.forEach((dot) => {
                dot.dataset.status = dotStatus;
                dot.dataset.state = dotStatus;
                dot.title = dotTitle;
            });

            setTextForAll(
                serverStatusMainTexts,
                mainText
            );

            setTextForAll(
                serverStatusDetailTexts,
                detailText
            );
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
            focusInputWithoutScroll();
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

            historyLoaded = true;
        }

        function updateApplicationState(state) {
            const serverRunning =
                state?.llama_server_running === true;

            const modelGenerating =
                state?.model_generates === true;

            if (Array.isArray(state?.assistant_profiles)) {
                profileState = new Map(
                    state.assistant_profiles
                        .filter((profile) => {
                            return typeof profile?.id === "string";
                        })
                        .map((profile) => {
                            return [profile.id, profile];
                        })
                );
            }

            const profileChanged = applyAssistantProfile(
                state?.assistant_profile
            );

            if (
                profileChanged &&
                historyLoaded &&
                !requestInFlight &&
                !profileChangeInFlight
            ) {
                clearRenderedMessages();

                loadHistory().catch((error) => {
                    console.error(
                        "Failed to load profile history:",
                        error
                    );
                });
            }

            setServerVisualState({
                running: serverRunning,
                generating: modelGenerating,
            });

            companyNameElements.forEach((element) => {
                if (
                    typeof state?.company_name === "string" &&
                    state.company_name.length !== 0
                ) {
                    element.textContent =
                        state.company_name;
                }
            });

            subscriptionElements.forEach((element) => {
                if (
                    typeof state?.subscription === "string" &&
                    state.subscription.length !== 0
                ) {
                    element.textContent =
                        state.subscription;

                    element.dataset.subscriptionPlan =
                        state.subscription;
                }
            });

            if (
                modelGenerating &&
                !requestInFlight
            ) {
                setGenerationState(
                    true,
                    {
                        scroll: false,
                        focus: false,
                    }
                );
            }

            if (
                previousServerGenerating &&
                !modelGenerating &&
                !requestInFlight
            ) {
                setGenerationState(
                    false,
                    {
                        focus: false,
                    }
                );

                loadHistory().catch((error) => {
                    console.error(
                        "Failed to reload history:",
                        error
                    );
                });
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

                setServerVisualState({
                    running: false,
                    offline: true,
                });
            }
        }

        async function submitMessage() {
            const rawUserMessage =
                input.value;

            const userMessage =
                activeAssistantProfile === "texting"
                    ? rawUserMessage
                    : rawUserMessage.trim();

            if (
                userMessage.trim().length === 0 ||
                generationActive ||
                requestInFlight ||
                restartRequestInFlight
            ) {
                return;
            }

            generationCancelledByUser = false;

            removeWelcomeMessage();

            appendUserMessage(
                userMessage,
                "pending"
            );

            clearInput();

            requestInFlight = true;

            setGenerationState(
                true,
                {
                    scroll: true,
                    focus: false,
                }
            );

            try {
                const payload =
                    await api.sendChatMessage(
                        userMessage
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

                if (!generationCancelledByUser) {
                    completeGeneration(answer);
                }
            } catch (error) {
                if (handleApiRedirect(error)) {
                    return;
                }

                console.error(
                    "Chat request failed:",
                    error
                );

                if (!generationCancelledByUser) {
                    failGeneration(
                        error?.message ??
                        "Не удалось получить ответ от бота."
                    );
                }
            } finally {
                requestInFlight = false;

                if (
                    generationActive &&
                    !generationCancelledByUser
                ) {
                    setGenerationState(
                        false,
                        {
                            focus: true,
                        }
                    );
                }

                if (generationCancelledByUser) {
                    generationActive = false;
                    generatingMessage.hidden = true;
                    messages.setAttribute(
                        "aria-busy",
                        "false"
                    );

                    sendButton.hidden = false;
                    stopButton.hidden = true;
                    input.disabled = false;

                    form.dataset.generating = "false";
                    form.dataset.stopping = "false";
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

            /*
             * Критично:
             * Не вызываем setGenerationState() здесь.
             * Не скрываем indicator.
             * Не двигаем generatingMessage.
             * Не фокусируем input.
             * Не делаем scrollMessagesToBottom().
             *
             * Иначе браузер пересчитывает scroll anchoring,
             * из-за чего появляется скачок вверх/вниз.
             */
            form.dataset.stopping = "true";

            stopButton.dataset.stopping = "true";
            stopButton.setAttribute("aria-disabled", "true");
            stopButton.title = "Останавливаю генерацию...";

            /*
             * Убираем фокус с кнопки, но не переносим его в textarea.
             * Перенос фокуса в input во время генерации тоже может дёрнуть viewport.
             */
            stopButton.blur();

            try {
                await api.stopGeneration();

                /*
                 * Ничего не скрываем.
                 *
                 * Ждём, пока основной POST /api/chat/messages
                 * вернётся из Application::ask() со статусом cancelled.
                 */
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

                form.dataset.stopping = "false";

                stopButton.dataset.stopping = "false";
                stopButton.setAttribute("aria-disabled", "false");
                stopButton.title = "Остановить генерацию ответа";
            }
        }

        async function restartModelServer() {
            if (
                restartRequestInFlight ||
                requestInFlight ||
                generationActive
            ) {
                appendAssistantMessage(
                    "Сначала дождитесь завершения текущего ответа или остановите генерацию.",
                    "failed"
                );

                closeComposerMenu();

                return;
            }

            const confirmed =
                window.confirm(
                    "Перезапустить локальную модель? На время перезапуска чат будет недоступен."
                );

            if (!confirmed) {
                closeComposerMenu();
                return;
            }

            restartRequestInFlight = true;
            closeComposerMenu();

            if (restartServerButton !== null) {
                restartServerButton.disabled = true;
            }

            input.disabled = true;
            sendButton.disabled = true;

            setServerVisualState({
                running: false,
                restarting: true,
            });

            try {
                const payload =
                    await api.restartModelServer();

                appendAssistantMessage(
                    payload?.message ??
                    "Модель перезапущена."
                );
            } catch (error) {
                console.error(
                    "Failed to restart model server:",
                    error
                );

                appendAssistantMessage(
                    error?.message ??
                    "Не удалось перезапустить модель.",
                    "failed"
                );
            } finally {
                restartRequestInFlight = false;

                if (restartServerButton !== null) {
                    restartServerButton.disabled = false;
                }

                sendButton.disabled = false;
                input.disabled = false;

                focusInputWithoutScroll();

                await refreshApplicationState();
            }
        }

        async function clearHistory() {
            if (
                generationActive ||
                requestInFlight ||
                restartRequestInFlight
            ) {
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
                appendWelcomeMessage();
                closeComposerMenu();
                focusInputWithoutScroll();
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

                if (event.shiftKey) {
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

        restartServerButton?.addEventListener(
            "click",
            restartModelServer
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

                if (
                    assistantProfileMenu.open &&
                    !assistantProfileMenu.contains(
                        event.target
                    )
                ) {
                    closeAssistantProfileMenu();
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
                            button.textContent
                                .split("\n")
                                .map((line) => line.trim())
                                .join("\n")
                                .trim();

                        resizeInput();
                        closeCheatsheets();
                        focusInputWithoutScroll();
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

        assistantProfileButtons.forEach((button) => {
            button.addEventListener(
                "click",
                () => {
                    changeAssistantProfile(
                        button.dataset.assistantProfile
                    );
                }
            );
        });

        document.addEventListener(
            "keydown",
            (event) => {
                if (event.key !== "Escape") {
                    return;
                }

                closeComposerMenu();
                closeAssistantProfileMenu();

                if (!cheatsheetsModal.hidden) {
                    closeCheatsheets();
                }
            }
        );

        resizeInput();

        setGenerationState(
            false,
            {
                focus: false,
            }
        );

        await refreshApplicationState();

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

        if (!generationActive && !input.disabled) {
            focusInputWithoutScroll();
        }

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
                restartModelServer,
            });
    }
);