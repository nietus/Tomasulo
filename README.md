# Simulador do Algoritmo de Tomasulo

Este projeto é um simulador em C++ do algoritmo de Tomasulo, uma técnica de hardware utilizada em processadores para permitir a execução fora de ordem de instruções e explorar o paralelismo em nível de instrução, melhorando o desempenho. O simulador demonstra os principais conceitos do algoritmo, como estações de reserva, renomeação de registradores e o Common Data Bus (CDB).

<p align="center">
  <img src='tomasulo_diagram.png' alt="Diagrama do Algoritmo de Tomasulo" width="500">
</p>
<p align="center">
  <em>Diagrama conceitual da arquitetura de Tomasulo (Fonte da imagem: Trabalho do usuário).</em>
</p>

## Funcionalidades Implementadas

- Leitura de instruções de um arquivo de texto.
- Simulação dos estágios do algoritmo de Tomasulo:
  - **Issue (Emissão):** Envio de instruções para estações de reserva, com tratamento de dependências de dados (RAW) e renomeação implícita de registradores.
  - **Execute (Execução):** Execução das instruções nas unidades funcionais (simuladas pelas estações de reserva) após a obtenção dos operandos, considerando suas latências.
  - **Write Result (Escrita de Resultado):** Transmissão do resultado via Common Data Bus (CDB) para o banco de registradores e para as estações de reserva que aguardam esse resultado.
- Suporte para instruções aritméticas (ADD, SUB, MUL, DIV) e de acesso à memória (L.D - Load, S.D - Store).
- Cálculo correto de endereço efetivo para L.D/S.D (`offset + valor_registrador_base`).
- Saída detalhada ciclo a ciclo mostrando o estado das instruções e das estações de reserva.
- Exibição dos valores finais dos registradores.

## Estrutura do Código

O simulador é implementado em C++ e organizado em torno da classe `TomasuloSimulator`, com estruturas auxiliares para representar os componentes do processador.

### Principais Estruturas de Dados:

- **`InstructionType` (enum):**
  Define os tipos de instruções suportadas: `ADD`, `SUB`, `MUL`, `DIV`, `LOAD`, `STORE`, `INVALID`.
- **`Instruction` (struct):**
  Representa uma instrução individual com os seguintes campos:

  - `type`: `InstructionType` da operação.
  - `dest`: String do registrador de destino.
  - `src1`: String do primeiro operando fonte (ou offset para L.D, ou dado para S.D).
  - `src2`: String do segundo operando fonte (ou registrador base para L.D/S.D).
  - `issue`: Ciclo de emissão.
  - `execComp`: Ciclo de conclusão da execução.
  - `writeResult`: Ciclo de escrita do resultado no CDB.

- **`ReservationStation` (struct):**
  Representa uma entrada em qualquer uma das estações de reserva (Add, Multiply/Divide, Load, Store).

  - `busy`: Booleano indicando se a RS está ocupada.
  - `op`: `InstructionType` da operação na RS.
  - `Vj`, `Vk`: Valores dos operandos fonte. `Vk` também armazena o valor do registrador base para L.D/S.D.
  - `Qj`, `Qk`: Nomes (tags) das estações de reserva que produzirão `Vj` e `Vk`, respectivamente. Se vazios, os valores em `Vj`/`Vk` estão prontos. `Qk` é usado para a tag do registrador base em L.D/S.D.
  - `dest`: Tag da própria RS (para aritmética e LOAD, identifica o destino da escrita). Para STORE, não é usado como destino de registrador.
  - `A`: Armazena o offset para instruções L.D/S.D.
  - `instructionIndex`: Índice da instrução original associada.

- **`RegisterStatus` (struct):**
  Mantém o estado de cada registrador para fins de renomeação.

  - `busy`: Booleano indicando se o registrador está aguardando um resultado.
  - `reorderName`: Tag da estação de reserva que irá escrever neste registrador.

### Classe `TomasuloSimulator`:

Contém toda a lógica da simulação.

- **Componentes Internos:**

  - `vector<Instruction> instructions`: Armazena todas as instruções do programa.
  - `vector<ReservationStation> addRS, mulRS, loadRS, storeRS`: Vetores para os diferentes tipos de estações de reserva.
  - `map<string, int> registers`: Simula o banco de registradores (ex: "F0" -> valor).
  - `map<string, RegisterStatus> regStatus`: Tabela de status dos registradores.
  - `int memory[1024]`: Array que simula a memória principal.
  - `int cycle`: Contador de ciclo atual.
  - `int nextInstructionIndex`: Ponteiro para a próxima instrução a ser emitida.
  - Constantes de latência (`ADD_LATENCY`, `MUL_LATENCY`, etc.).
  - `vector<ExecutingInstruction> executingInstructions`: Lista de instruções atualmente em unidades funcionais.
  - `queue<int> completedInstructions`: Fila para instruções que terminaram a execução e aguardam o CDB.

- **Métodos Principais da Simulação (Fases do Tomasulo):**

  1. **`loadInstructions(const string& filename)`:**
     Lê e parseia as instruções de um arquivo texto. Identifica o tipo de operação, operandos destino e fonte. Para L.D e S.D, parseia o formato `offset(RegistradorBase)`.
  2. **`issueInstruction()` (Estágio de Emissão):**
     - Verifica se há uma estação de reserva (RS) livre do tipo apropriado.
     - Se sim, a próxima instrução (`instructions[nextInstructionIndex]`) é emitida para essa RS.
     - Os campos da RS são preenchidos: `Op`, `Busy`, `instructionIndex`.
     - Para os operandos fonte (`src1`, `src2`):
       - Verifica o `regStatus`. Se o registrador fonte estiver `busy`, o campo `Qj` ou `Qk` da RS recebe a `reorderName` (tag da RS que produzirá o valor).
       - Se o registrador fonte estiver disponível, seu valor é copiado para `Vj` ou `Vk`.
     - Para L.D/S.D:
       - O `offset` é armazenado em `rs->A`.
       - O registrador base é tratado como um operando fonte, populando `Vk` ou `Qk`.
       - Para S.D, o registrador contendo o dado a ser armazenado também é tratado como um operando fonte, populando `Vj` ou `Qj`.
     - O `regStatus` do registrador de destino da instrução (se não for STORE) é atualizado para `busy=true` e `reorderName` aponta para a tag da RS atual.
  3. **`startExecution()` (Início da Execução):**
     - Itera por todas as RSs ocupadas.
     - Se uma RS tem todos os seus operandos disponíveis (`Qj` e `Qk` vazios), a instrução associada é movida para a lista `executingInstructions`.
     - O número de ciclos restantes para execução é definido com base na latência da operação.
  4. **`advanceExecution()` (Avanço da Execução):**
     - Para cada instrução em `executingInstructions`, decrementa `remainingCycles`.
     - Se `remainingCycles` chega a zero, a instrução completou a execução. Seu ciclo de `execComp` é registrado e ela é movida para a fila `completedInstructions` (aguardando o CDB).
  5. **`processWriteBack()` (Escrita de Resultado via CDB):**
     - Se a fila `completedInstructions` não estiver vazia, uma instrução é retirada (simulando um CDB).
     - O ciclo de `writeResult` é registrado.
     - O resultado da operação é calculado (ou lido da memória para L.D).
     - Se a instrução não for um `STORE`, o resultado é escrito no registrador de destino no banco `registers`. O `regStatus` do registrador de destino é liberado se esta RS era a que estava produzindo seu valor.
     - O nome da RS produtora e o valor do resultado são transmitidos para todas as outras estações de reserva (`updateDependentRS`).
     - Para `STORE`, o valor do operando `Vj` (dado) é escrito na posição de memória calculada (`A + Vk`).
     - A RS que completou é marcada como não `busy`.
  6. **`updateDependentRS(const string &producingRSName, int resultValue)`:**
     - Itera por todas as RSs ocupadas.
     - Se o campo `Qj` ou `Qk` de uma RS corresponde a `producingRSName`, o `Vj` ou `Vk` correspondente recebe o `resultValue` e o `Qj` ou `Qk` é limpo, indicando que o operando agora está disponível.

- **Fluxo da Simulação (`stepSimulation()`):**
  A ordem de chamada das fases dentro de um ciclo é importante para refletir corretamente o fluxo de dados:

  1. `processWriteBack()`: Para liberar recursos e disponibilizar resultados.
  2. `issueInstruction()`: Para tentar emitir novas instruções.
  3. `startExecution()`: Para iniciar a execução de instruções que ficaram prontas (possivelmente devido ao WriteBack).
  4. `advanceExecution()`: Para progredir as instruções já em execução.
     O ciclo então é incrementado.

- **Saída (`printStatus()`, `printRegisters()`):**

  - `printStatus()`: Exibe o estado detalhado de todas as instruções (ciclos de issue, execComp, writeResult) e o conteúdo de cada entrada em todas as estações de reserva (Add, Mul/Div, Load, Store), incluindo seus campos `Busy`, `Op`, `Vj`, `Vk`, `Qj`, `Qk`, `Dest`, `A`, `InstIdx`.
  - `printRegisters()`: Mostra os valores finais de todos os registradores após a conclusão da simulação.

## Como Compilar e Executar

1. **Compilação:**
   Use um compilador C++ (como g++). É recomendável usar o padrão C++11 ou superior para melhor compatibilidade com todas as features da linguagem utilizadas (como `std::to_string`, embora a versão atual use `stringstream` para maior portabilidade).

   ```bash
   g++ tomasulo.cpp -o tomasulo -std=c++11
   ```

2. **Execução:**
   Execute o programa compilado. Ele solicitará o nome do arquivo de instruções.

   ```bash
   ./tomasulo
   ```

   Quando solicitado, digite o nome do arquivo (ex: `instructions.txt`, `instructions1.txt`, `instructions2.txt`).

   O programa irá executar ciclo a ciclo, mostrando o status. Pressione ENTER para avançar para o próximo ciclo.

## Formato do Arquivo de Instruções (`.txt`)

As instruções devem ser escritas uma por linha. Comentários podem ser adicionados usando `#` no início da linha.
Os registradores são nomeados como `F0`, `F1`, ..., `F31`.
Os valores iniciais dos registradores são `10`. A memória é inicializada com `memory[i] = i`.

**Formatos suportados:**

- **Aritméticas:**
  - `ADD Fdest,Fsrc1,Fsrc2`
  - `SUB Fdest,Fsrc1,Fsrc2`
  - `MUL Fdest,Fsrc1,Fsrc2`
  - `DIV Fdest,Fsrc1,Fsrc2`
- **Load:**
  - `L.D Fdest,offset(Fbase)` (ex: `L.D F2,100(F0)`)
  - `LOAD Fdest,offset(Fbase)` (alternativa)
- **Store:**
  - `S.D FsrcData,offset(Fbase)` (ex: `S.D F15,200(F1)`)
  - `STORE FsrcData,offset(Fbase)` (alternativa)

**Latências Padrão (Configuradas no Código):**

- ADD/SUB: 2 ciclos
- MUL: 10 ciclos
- DIV: 40 ciclos
- LOAD/STORE: 2 ciclos

## Exemplo de Execução (com `instructions.txt`)

Arquivo `instructions.txt`:

```
ADD F1, F2, F3
SUB F4, F1, F5
MUL F6, F4, F1
```

Considerando os valores iniciais dos registradores como 10:

- `F1 = F2(10) + F3(10) = 20`
- `F4 = F1(20) - F5(10) = 10`
- `F6 = F4(10) * F1(20) = 200`

**Valores Finais Esperados:**

- F1 = 20
- F4 = 10
- F6 = 200
- Demais registradores (F0, F2, F3, F5, etc.) = 10 (valor inicial).
