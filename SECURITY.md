# Безопасность

## ⚠️ ВАЖНО: Никогда не коммитьте токены в Git!

GitHub Personal Access Tokens должны храниться ТОЛЬКО локально и НИКОГДА не должны попадать в репозиторий.

## Правильное хранение credentials

### Вариант 1: Глобальный gradle.properties (РЕКОМЕНДУЕТСЯ)

Создайте файл в домашней директории:

**Windows:**

```
C:\Users\YOUR_USERNAME\.gradle\gradle.properties
```

**Linux/Mac:**

```
~/.gradle/gradle.properties
```

Содержимое:

```properties
gpr.user=Daronec
gpr.key=YOUR_NEW_TOKEN_HERE
```

### Вариант 2: Переменные окружения

**Windows (CMD):**

```cmd
setx GPR_USER "Daronec"
setx GPR_KEY "YOUR_NEW_TOKEN_HERE"
```

**Windows (PowerShell):**

```powershell
[System.Environment]::SetEnvironmentVariable('GPR_USER', 'Daronec', 'User')
[System.Environment]::SetEnvironmentVariable('GPR_KEY', 'YOUR_NEW_TOKEN_HERE', 'User')
```

**Linux/Mac:**

```bash
echo 'export GPR_USER=Daronec' >> ~/.bashrc
echo 'export GPR_KEY=YOUR_NEW_TOKEN_HERE' >> ~/.bashrc
source ~/.bashrc
```

## Что делать, если токен утек?

1. **НЕМЕДЛЕННО удалите токен:**
   - Перейдите на https://github.com/settings/tokens
   - Найдите скомпрометированный токен
   - Нажмите "Delete" или "Revoke"

2. **Создайте новый токен:**
   - https://github.com/settings/tokens/new
   - Выберите минимальные необходимые права
   - Сохраните в безопасном месте

3. **Проверьте историю Git:**

   ```bash
   # Проверьте, не был ли токен закоммичен
   git log --all --full-history -- "*gradle.properties"
   ```

4. **Если токен был закоммичен:**

   ```bash
   # Удалите файл из истории Git
   git filter-branch --force --index-filter \
     "git rm --cached --ignore-unmatch gradle/gradle.properties" \
     --prune-empty --tag-name-filter cat -- --all

   # Принудительно отправьте изменения
   git push origin --force --all
   ```

## .gitignore

Убедитесь, что в `.gitignore` есть:

```gitignore
# Credentials
gradle.properties
local.properties
*.properties

# Secrets
secrets/
*.key
*.pem
```

## GitHub Actions

Для CI/CD используйте GitHub Secrets:

1. Перейдите в Settings → Secrets and variables → Actions
2. Добавьте секреты:
   - `GPR_USER`: ваш username
   - `GPR_KEY`: ваш token

В workflow используйте:

```yaml
env:
  GPR_USER: ${{ secrets.GPR_USER }}
  GPR_KEY: ${{ secrets.GPR_KEY }}
```

## Проверка перед коммитом

Всегда проверяйте, что не коммитите секреты:

```bash
# Проверьте staged файлы
git diff --cached

# Проверьте на наличие токенов
git diff --cached | grep -i "ghp_"
git diff --cached | grep -i "token"
git diff --cached | grep -i "password"
```

## Инструменты для проверки

- [git-secrets](https://github.com/awslabs/git-secrets) - предотвращает коммит секретов
- [truffleHog](https://github.com/trufflesecurity/trufflehog) - сканирует репозиторий на секреты
- [GitHub Secret Scanning](https://docs.github.com/en/code-security/secret-scanning) - автоматическое сканирование

## Контрольный список безопасности

- [ ] Токены хранятся в `~/.gradle/gradle.properties` (НЕ в проекте)
- [ ] `gradle.properties` добавлен в `.gitignore`
- [ ] Проверена история Git на наличие токенов
- [ ] Настроены GitHub Secrets для CI/CD
- [ ] Токены имеют минимальные необходимые права
- [ ] Токены регулярно обновляются (каждые 90 дней)

## Сообщение об уязвимости

Если вы обнаружили уязвимость в безопасности, пожалуйста:

1. НЕ создавайте публичный Issue
2. Отправьте email на [ваш email]
3. Опишите проблему и шаги для воспроизведения
