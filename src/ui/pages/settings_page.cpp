#include "settings_page.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "../../db/repositories.h"

static const char *kQualityKey = "quality";
static const char *kLanguageKey = "language";

static const int kDefaultQuality = 720;

static QStringList qualityOptions()
{
    return {QStringLiteral("360"), QStringLiteral("480"),
            QStringLiteral("720"), QStringLiteral("1080")};
}

static QStringList languageOptions()
{
    return {QStringLiteral("Español"), QStringLiteral("English"),
            QStringLiteral("Português"), QStringLiteral("Español (Latinoamérica)")};
}

SettingsPage::SettingsPage(SettingsRepo *settings, QWidget *parent)
    : QWidget(parent)
    , m_settings(settings)
{
    buildUI();
}

void SettingsPage::buildUI()
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(20, 20, 20, 20);
    lay->setSpacing(16);

    auto *title = new QLabel(QStringLiteral("Ajustes"), this);
    title->setStyleSheet(QStringLiteral("font-size:18px; font-weight:700;"));
    lay->addWidget(title);

    auto *qualityRow = new QHBoxLayout;
    qualityRow->setSpacing(8);
    auto *qualityLabel = new QLabel(QStringLiteral("Calidad predeterminada"), this);
    qualityLabel->setStyleSheet(QStringLiteral("color:#8F8F8F; min-width:180px;"));
    m_qualityCombo = new QComboBox(this);
    m_qualityCombo->addItems(qualityOptions());
    const int qIdx = qualityOptions().indexOf(
        QString::number(m_settings->getInt(kQualityKey, kDefaultQuality)));
    m_qualityCombo->setCurrentIndex(qIdx >= 0 ? qIdx : 2);
    connect(m_qualityCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        m_settings->set(kQualityKey, m_qualityCombo->itemText(idx));
    });
    qualityRow->addWidget(qualityLabel);
    qualityRow->addWidget(m_qualityCombo, 1);
    lay->addLayout(qualityRow);

    auto *langRow = new QHBoxLayout;
    langRow->setSpacing(8);
    auto *langLabel = new QLabel(QStringLiteral("Idioma de la interfaz"), this);
    langLabel->setStyleSheet(QStringLiteral("color:#8F8F8F; min-width:180px;"));
    m_languageCombo = new QComboBox(this);
    m_languageCombo->addItems(languageOptions());
    const int lIdx = languageOptions().indexOf(
        m_settings->getString(kLanguageKey, QStringLiteral("Español")));
    m_languageCombo->setCurrentIndex(lIdx >= 0 ? lIdx : 0);
    connect(m_languageCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        m_settings->set(kLanguageKey, m_languageCombo->itemText(idx));
    });
    langRow->addWidget(langLabel);
    langRow->addWidget(m_languageCombo, 1);
    lay->addLayout(langRow);

    lay->addStretch(1);
}
