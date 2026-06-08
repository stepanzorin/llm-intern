"use strict";

document.addEventListener("DOMContentLoaded", () => {
    const storageKeys =
        window.internUi.storageKeys;

    const subscriptionElement =
        document.getElementById("subscription-name");

    const form =
        document.getElementById("settings-form");

    const savedMessage =
        document.getElementById("settings-saved");

    const knowledgeHelpModal =
        document.getElementById(
            "knowledge-help-modal"
        );

    const customThemeOption =
        document.getElementById(
            "custom-theme-option"
        );

    const customThemeInput =
        document.getElementById(
            "custom-theme-input"
        );

    const themePlanMessage =
        document.getElementById(
            "theme-plan-message"
        );

    const modelSelect =
        document.getElementById("model-select");

    const modelPlanHint =
        document.getElementById(
            "model-plan-hint"
        );

    const exportHistoryButton =
        document.getElementById(
            "export-history"
        );

    const knowledgeInput =
        document.getElementById(
            "knowledge-files"
        );

    const knowledgeDropzone =
        document.getElementById(
            "knowledge-upload-dropzone"
        );

    const knowledgeUploadIcon =
        document.getElementById(
            "knowledge-upload-icon"
        );

    const knowledgeUploadTitle =
        document.getElementById(
            "knowledge-upload-title"
        );

    const knowledgeUploadDescription =
        document.getElementById(
            "knowledge-upload-description"
        );

    const knowledgeLimitMessage =
        document.getElementById(
            "knowledge-limit-message"
        );

    const knowledgeValidationMessage =
        document.getElementById(
            "knowledge-validation-message"
        );

    const knowledgeFilesList =
        document.getElementById(
            "knowledge-files-list"
        );

    const knowledgeEmptyState =
        document.getElementById(
            "knowledge-empty-state"
        );

    const planOrder = Object.freeze({
        FREE: 0,
        PLUS: 1,
        PREMIUM: 2,
    });

    const knowledgeLimits = Object.freeze({
        FREE: 3,
        PLUS: 20,
        PREMIUM: 20,
    });

    let currentPlan =
        window.internUi.normalizeSubscriptionPlan(
            subscriptionElement
                ?.dataset
                .subscriptionPlan
        );

    const knowledgeFiles = [];

    function planAllows(requiredPlan) {
        return (
            planOrder[currentPlan.code] >=
            planOrder[requiredPlan]
        );
    }

    function currentKnowledgeLimit() {
        return (
            knowledgeLimits[currentPlan.code] ??
            knowledgeLimits.FREE
        );
    }

    function showElementTemporarily(
        element,
        durationMilliseconds = 3500
    ) {
        element.hidden = false;

        window.clearTimeout(
            Number(element.dataset.hideTimer ?? 0)
        );

        const timeoutId =
            window.setTimeout(() => {
                element.hidden = true;
            }, durationMilliseconds);

        element.dataset.hideTimer =
            String(timeoutId);
    }

    function showValidationMessage(message) {
        knowledgeValidationMessage.textContent =
            message;

        showElementTemporarily(
            knowledgeValidationMessage
        );
    }

    function updateSubscriptionLabel() {
        subscriptionElement.dataset.subscriptionPlan =
            currentPlan.code;

        subscriptionElement.textContent =
            window.internUi.subscriptionPlanLabel(
                currentPlan.code
            );
    }

    function updateFeatureAccess() {
        const customThemeAllowed =
            planAllows("PLUS");

        customThemeInput.disabled =
            !customThemeAllowed;

        customThemeOption.dataset.locked =
            String(!customThemeAllowed);

        customThemeOption.classList.toggle(
            "theme-option--locked",
            !customThemeAllowed
        );

        if (
            !customThemeAllowed &&
            customThemeInput.checked
        ) {
            const lightThemeInput =
                document.querySelector(
                    'input[name="theme"][value="light"]'
                );

            lightThemeInput.checked = true;

            localStorage.setItem(
                storageKeys.theme,
                "light"
            );

            window.internUi.applyStoredTheme();
        }

        const modelSelectionAllowed =
            planAllows("PREMIUM");

        modelSelect.disabled =
            !modelSelectionAllowed;

        modelPlanHint.hidden =
            modelSelectionAllowed;

        exportHistoryButton.disabled =
            !modelSelectionAllowed;
    }

    function makeKnowledgeFileKey(file) {
        return [
            file.name,
            file.size,
            file.lastModified,
        ].join(":");
    }

    function isMarkdownFile(file) {
        const filename =
            file.name.toLowerCase();

        return (
            filename.endsWith(".md") ||
            filename.endsWith(".markdown")
        );
    }

    function formatFileSize(sizeBytes) {
        if (sizeBytes < 1024) {
            return `${sizeBytes} Б`;
        }

        const sizeKilobytes =
            sizeBytes / 1024;

        if (sizeKilobytes < 1024) {
            return `${sizeKilobytes.toFixed(1)} КБ`;
        }

        const sizeMegabytes =
            sizeKilobytes / 1024;

        return `${sizeMegabytes.toFixed(1)} МБ`;
    }

    function removeKnowledgeFile(fileKey) {
        const index =
            knowledgeFiles.findIndex(
                (item) => item.key === fileKey
            );

        if (index === -1) {
            return;
        }

        knowledgeFiles.splice(index, 1);

        renderKnowledgeFiles();
        updateKnowledgeLimitState();

        window.dispatchEvent(
            new CustomEvent(
                "settings:knowledge-files-change",
                {
                    detail: {
                        files: knowledgeFiles.map(
                            (item) => item.file
                        ),
                    },
                }
            )
        );
    }

    function makeKnowledgeFileElement(item) {
        const article =
            document.createElement("article");

        article.className =
            "knowledge-file";

        const information =
            document.createElement("div");

        const filename =
            document.createElement("strong");

        filename.textContent =
            item.file.name;

        const filesize =
            document.createElement("span");

        filesize.textContent =
            formatFileSize(item.file.size);

        information.append(
            filename,
            filesize
        );

        const removeButton =
            document.createElement("button");

        removeButton.type = "button";
        removeButton.ariaLabel =
            `Удалить ${item.file.name}`;

        removeButton.textContent = "✕";

        removeButton.addEventListener(
            "click",
            () => {
                removeKnowledgeFile(item.key);
            }
        );

        article.append(
            information,
            removeButton
        );

        return article;
    }

    function renderKnowledgeFiles() {
        knowledgeFilesList.replaceChildren();

        for (const item of knowledgeFiles) {
            knowledgeFilesList.append(
                makeKnowledgeFileElement(item)
            );
        }

        const hasFiles =
            knowledgeFiles.length !== 0;

        knowledgeFilesList.hidden =
            !hasFiles;

        knowledgeEmptyState.hidden =
            hasFiles;
    }

    function updateKnowledgeLimitState() {
        const limit =
            currentKnowledgeLimit();

        const limitReached =
            knowledgeFiles.length >= limit;

        knowledgeInput.disabled =
            limitReached;

        knowledgeDropzone.classList.toggle(
            "knowledge-upload__dropzone--locked",
            limitReached
        );

        knowledgeDropzone.dataset.locked =
            String(limitReached);

        if (!limitReached) {
            knowledgeUploadIcon.textContent = "+";

            knowledgeUploadTitle.textContent =
                "Добавить Markdown-файлы";

            knowledgeUploadDescription.textContent =
                "Перетащите файлы сюда или нажмите для выбора.";

            knowledgeLimitMessage.hidden = true;

            return;
        }

        knowledgeUploadIcon.textContent = "🔒";

        if (currentPlan.code === "FREE") {
            knowledgeUploadTitle.textContent =
                "Достигнут лимит тарифа FREE";

            knowledgeUploadDescription.textContent =
                "Для добавления четвёртого файла нужен тариф PLUS.";

            return;
        }

        knowledgeUploadTitle.textContent =
            "Достигнут лимит базы знаний";

        knowledgeUploadDescription.textContent =
            "Можно добавить не более двадцати файлов.";
    }

    function addKnowledgeFiles(files) {
        const limit =
            currentKnowledgeLimit();

        let limitWasReached = false;
        let invalidFilesFound = false;
        let duplicateFilesFound = false;

        for (const file of files) {
            if (!isMarkdownFile(file)) {
                invalidFilesFound = true;
                continue;
            }

            const key =
                makeKnowledgeFileKey(file);

            const alreadyExists =
                knowledgeFiles.some(
                    (item) => item.key === key
                );

            if (alreadyExists) {
                duplicateFilesFound = true;
                continue;
            }

            if (
                knowledgeFiles.length >= limit
            ) {
                limitWasReached = true;
                break;
            }

            knowledgeFiles.push({
                key,
                file,
            });
        }

        knowledgeInput.value = "";

        renderKnowledgeFiles();
        updateKnowledgeLimitState();

        if (invalidFilesFound) {
            showValidationMessage(
                "Можно добавлять только файлы .md и .markdown."
            );
        } else if (duplicateFilesFound) {
            showValidationMessage(
                "Некоторые выбранные файлы уже были добавлены."
            );
        }

        if (limitWasReached) {
            if (currentPlan.code === "FREE") {
                showElementTemporarily(
                    knowledgeLimitMessage,
                    6000
                );
            } else {
                showValidationMessage(
                    "Достигнут общий лимит: 20 Markdown-файлов."
                );
            }
        }

        window.dispatchEvent(
            new CustomEvent(
                "settings:knowledge-files-change",
                {
                    detail: {
                        files: knowledgeFiles.map(
                            (item) => item.file
                        ),
                    },
                }
            )
        );
    }

    function openKnowledgeHelp() {
        knowledgeHelpModal.hidden = false;

        knowledgeHelpModal
            .querySelector(".modal__close")
            ?.focus();
    }

    function closeKnowledgeHelp() {
        knowledgeHelpModal.hidden = true;
    }

    function downloadKnowledgeTemplate() {
        const content = `# Название инструкции

## Частые запросы пользователя

пример запроса пользователя
другая формулировка запроса

## Пошаговая инструкция что делать

Важно:

* Первое важное правило.
* Второе важное правило.

Инструкция:

1. Первый шаг.
2. Второй шаг.
3. Проверка результата.
`;

        const blob = new Blob(
            [content],
            {
                type: "text/markdown;charset=utf-8",
            }
        );

        const url =
            URL.createObjectURL(blob);

        const link =
            document.createElement("a");

        link.href = url;
        link.download =
            "custom_instruction_template.md";

        document.body.append(link);

        link.click();
        link.remove();

        URL.revokeObjectURL(url);
    }

    function setSubscriptionPlan(planCode) {
        currentPlan =
            window.internUi.normalizeSubscriptionPlan(
                planCode
            );

        updateSubscriptionLabel();
        updateFeatureAccess();
        updateKnowledgeLimitState();
    }

    const savedTheme =
        localStorage.getItem(
            storageKeys.theme
        ) ?? "light";

    const savedThemeRadio =
        document.querySelector(
            `input[name="theme"][value="${savedTheme}"]`
        );

    if (savedThemeRadio !== null) {
        savedThemeRadio.checked = true;
    }

    customThemeOption.addEventListener(
        "click",
        (event) => {
            if (planAllows("PLUS")) {
                return;
            }

            event.preventDefault();

            showElementTemporarily(
                themePlanMessage
            );
        }
    );

    document
        .querySelectorAll(
            'input[name="theme"]'
        )
        .forEach((radio) => {
            radio.addEventListener(
                "change",
                () => {
                    if (!radio.checked) {
                        return;
                    }

                    localStorage.setItem(
                        storageKeys.theme,
                        radio.value
                    );

                    window.internUi.applyStoredTheme();
                }
            );
        });

    knowledgeDropzone.addEventListener(
        "click",
        (event) => {
            if (
                knowledgeFiles.length <
                currentKnowledgeLimit()
            ) {
                return;
            }

            event.preventDefault();

            if (currentPlan.code === "FREE") {
                showElementTemporarily(
                    knowledgeLimitMessage,
                    6000
                );

                return;
            }

            showValidationMessage(
                "Достигнут общий лимит: 20 Markdown-файлов."
            );
        }
    );

    knowledgeInput.addEventListener(
        "change",
        () => {
            addKnowledgeFiles(
                Array.from(
                    knowledgeInput.files ?? []
                )
            );
        }
    );

    document
        .querySelector(
            '[data-action="open-knowledge-help"]'
        )
        ?.addEventListener(
            "click",
            openKnowledgeHelp
        );

    document
        .querySelectorAll(
            '[data-action="close-knowledge-help"]'
        )
        .forEach((button) => {
            button.addEventListener(
                "click",
                closeKnowledgeHelp
            );
        });

    document
        .querySelectorAll(
            '[data-action="download-knowledge-template"]'
        )
        .forEach((button) => {
            button.addEventListener(
                "click",
                downloadKnowledgeTemplate
            );
        });

    form.addEventListener(
        "submit",
        (event) => {
            event.preventDefault();

            const selectedTheme =
                document.querySelector(
                    'input[name="theme"]:checked'
                );

            if (selectedTheme !== null) {
                localStorage.setItem(
                    storageKeys.theme,
                    selectedTheme.value
                );

                window.internUi.applyStoredTheme();
            }

            savedMessage.hidden = false;

            window.setTimeout(() => {
                savedMessage.hidden = true;
            }, 2500);
        }
    );

    document.addEventListener(
        "keydown",
        (event) => {
            if (
                event.key === "Escape" &&
                !knowledgeHelpModal.hidden
            ) {
                closeKnowledgeHelp();
            }
        }
    );

    updateSubscriptionLabel();
    updateFeatureAccess();
    renderKnowledgeFiles();
    updateKnowledgeLimitState();

    window.settingsPage = Object.freeze({
        setSubscriptionPlan,

        knowledgeFiles() {
            return knowledgeFiles.map(
                (item) => item.file
            );
        },
    });
});