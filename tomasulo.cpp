// Simulador do Algoritmo de Tomasulo - versão ROB

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream> // istringstream, stringstream
#include <map>     // std::map
#include <queue>   // std::queue
#include <iomanip> // setw, left, right
#include <limits>  // std::numeric_limits (limpar buffer de entrada)
#include <cstdlib> // atoi

using namespace std;

// Enumeração para os tipos de instruções suportadas.
enum InstructionType
{
    ADD,
    SUB,
    MUL,
    DIV,
    LOAD,
    STORE,
    INVALID
};

// Enumeração para os estados de uma entrada no Reorder Buffer (ROB) - Controla o progresso da instrução desde a emissão até o commit.
enum ROBState
{
    ROB_EMPTY,       // A entrada do ROB está livre (não está sendo usada).
    ROB_ISSUE,       // A instrução foi emitida e alocada no ROB; aguarda na RS ou operandos.
    ROB_EXECUTE,     // A instrução está atualmente em execução em uma unidade funcional.
    ROB_WRITERESULT, // O resultado da instrução está pronto e foi escrito nesta entrada do ROB (via CDB), aguardando o commit.
    // ROB_COMMIT é um estado transitório; após o commit, a entrada volta para ROB_EMPTY.
};

// Estrutura para representar uma instrução individual lida do arquivo de entrada - Contém a definição da operação e rastreia os ciclos dos estágios chave.
struct Instruction
{
    InstructionType type; // Tipo da operação (ex: ADD, LOAD).
    string dest;          // Registrador de destino (ex: "F1"). Para STORE, armazena o offset.
    string src1;          // Primeiro operando fonte. Para LOAD, é o offset; para STORE, é o registrador do dado.
    string src2;          // Segundo operando fonte. Para LOAD/STORE, é o registrador base.
    int issue = -1;       // Ciclo em que a instrução foi emitida.
    int execComp = -1;    // Ciclo em que a execução foi concluída.
    int writeResult = -1; // Ciclo em que o resultado foi escrito no ROB (via CDB).
    int commitCycle = -1; // Ciclo em que a instrução foi cometida (commit).
};

// Estrutura para representar uma entrada no Reorder Buffer (ROB). O ROB é crucial para implementar a terminação em ordem e o tratamento preciso de exceções.
struct ReorderBufferEntry
{
    bool busy = false;              // Indica se esta entrada do ROB está atualmente em uso.
    int instructionIndex = -1;      // Índice da instrução original (no vetor 'instructions') associada a esta entrada.
    InstructionType type = INVALID; // Tipo da instrução.
    ROBState state = ROB_EMPTY;     // Estado atual da instrução dentro do ROB.
    string destinationRegister;     // Nome do registrador de destino arquitetural (ex: "F1"). Vazio para STORE.
    int value = 0;                  // Valor calculado (para ALU/LOAD) ou valor a ser armazenado (para STORE).
    int address = 0;                // Endereço de memória calculado (relevante para LOAD/STORE).
    bool valueReady = false;        // True se 'value' (resultado/dado do store) está pronto nesta entrada do ROB.
};

// Estrutura para representar uma entrada na Estação de Reserva (Reservation Station - RS). As RSs guardam instruções que aguardam operandos ou a disponibilidade de uma unidade funcional.
struct ReservationStation
{
    bool busy = false;            // Indica se a RS está ocupada.
    InstructionType op = INVALID; // Tipo da operação sendo processada.
    int Vj = 0, Vk = 0;           // Valores dos operandos fonte (Vj para src1, Vk para src2/base).
    string Qj = "", Qk = "";      // Tags (índices do ROB como string) das entradas do ROB que produzirão Vj e Vk.
                                  // Se vazios, os valores em Vj/Vk estão disponíveis.
    string destRobIndex = "";     // Índice (como string) da entrada do ROB para a qual esta RS escreverá seu resultado.
    int A = 0;                    // Armazena o offset para instruções L.D/S.D.
    int instructionIndex = -1;    // Índice da instrução original associada.
};

// Estrutura para representar o status de um registrador arquitetural. Usada para renomeação: indica se o valor do registrador está pendente e qual entrada do ROB o produzirá.
struct RegisterStatus
{
    bool busy = false; // True se o registrador está aguardando um resultado de uma entrada do ROB.
    int robIndex = -1; // Índice da entrada do ROB que produzirá o valor para este registrador.
                       // -1 se o valor do registrador estiver no banco de registradores e não pendente.
};

// Classe principal do simulador Tomasulo.
class TomasuloSimulator
{
private:
    // --- Componentes Centrais do Processador Simulado ---
    vector<Instruction> instructions;                         // Todas as instruções lidas do programa.
    vector<ReservationStation> addRS, mulRS, loadRS, storeRS; // Estações de Reserva por tipo.
    vector<ReorderBufferEntry> rob;                           // O Reorder Buffer.
    map<string, int> registers;                               // Banco de registradores arquiteturais.
    map<string, RegisterStatus> regStatus;                    // Tabela de status dos registradores.
    int memory[1024];                                         // Memória principal simulada.

    // --- Controle da Simulação ---
    int cycle = 0;                // Ciclo atual da simulação.
    int nextInstructionIndex = 0; // Índice da próxima instrução a ser emitida.
    int robHead = 0, robTail = 0; // Ponteiros para a cabeça e cauda do ROB (fila circular).
    int robEntriesAvailable;      // Número de entradas livres no ROB.

    // --- Parâmetros de Configuração ---
    const int ADD_LATENCY, MUL_LATENCY, DIV_LATENCY, LOAD_LATENCY, STORE_LATENCY;
    const int ROB_SIZE;

    // --- Estruturas de Suporte para Execução ---
    // Rastreia instruções atualmente nas unidades funcionais (após saírem da RS, antes do CDB).
    struct ExecutingInstruction
    {
        int rsIndex;
        string rsType;
        int remainingCycles;
        int instructionIndex;
    };
    vector<ExecutingInstruction> executingInstructions;
    // Fila de instruções (índices originais) que completaram a execução e aguardam o CDB para escrever no ROB.
    queue<int> completedForCDB;

    // Encontra uma Estação de Reserva (RS) livre para um dado tipo de instrução.
    pair<int, string> findFreeRS(InstructionType type)
    {
        if (type == ADD || type == SUB)
        {
            for (size_t i = 0; i < addRS.size(); ++i)
                if (!addRS[i].busy)
                    return {static_cast<int>(i), "ADD"};
        }
        else if (type == MUL || type == DIV)
        {
            for (size_t i = 0; i < mulRS.size(); ++i)
                if (!mulRS[i].busy)
                    return {static_cast<int>(i), "MUL"};
        }
        else if (type == LOAD)
        {
            for (size_t i = 0; i < loadRS.size(); ++i)
                if (!loadRS[i].busy)
                    return {static_cast<int>(i), "LOAD"};
        }
        else if (type == STORE)
        {
            for (size_t i = 0; i < storeRS.size(); ++i)
                if (!storeRS[i].busy)
                    return {static_cast<int>(i), "STORE"};
        }
        return {-1, ""}; // Nenhuma RS livre do tipo especificado.
    }

    // Estágio de Emissão (Issue): aloca recursos (ROB, RS) e busca operandos.
    bool issueInstruction()
    {
        // Verifica se há instruções para emitir e se há espaço no ROB.
        if (nextInstructionIndex >= static_cast<int>(instructions.size()) || robEntriesAvailable == 0)
        {
            return false;
        }
        Instruction &originalInst = instructions[nextInstructionIndex]; // Próxima instrução do programa.

        // Tenta encontrar uma RS livre.
        pair<int, string> rsInfo = findFreeRS(originalInst.type);
        if (rsInfo.first == -1)
        {
            return false; // Emperramento estrutural: sem RS livre.
        }

        // 1. Alocar entrada no ROB.
        int currentRobIdx = robTail;
        ReorderBufferEntry &robEntry = rob[currentRobIdx];
        robEntry.busy = true;
        robEntry.instructionIndex = nextInstructionIndex;
        robEntry.type = originalInst.type;
        robEntry.state = ROB_ISSUE;                                                           // Estado inicial no ROB.
        robEntry.destinationRegister = (originalInst.type != STORE) ? originalInst.dest : ""; // STORE não tem reg. destino arquitetural.
        robEntry.value = 0;
        robEntry.address = 0;
        robEntry.valueReady = false; // Inicializa campos.

        robTail = (robTail + 1) % ROB_SIZE; // Avança a cauda do ROB.
        robEntriesAvailable--;
        originalInst.issue = cycle; // Registra ciclo de emissão.

        // 2. Preencher a Estação de Reserva (RS) alocada.
        ReservationStation *rs = nullptr;
        if (rsInfo.second == "ADD")
            rs = &addRS[rsInfo.first];
        else if (rsInfo.second == "MUL")
            rs = &mulRS[rsInfo.first];
        else if (rsInfo.second == "LOAD")
            rs = &loadRS[rsInfo.first];
        else if (rsInfo.second == "STORE")
            rs = &storeRS[rsInfo.first];
        // Não deve ser nullptr se rsInfo.first != -1.

        rs->busy = true;
        rs->op = originalInst.type;
        rs->instructionIndex = nextInstructionIndex;
        rs->destRobIndex = to_string(currentRobIdx); // A RS agora sabe para qual entrada do ROB deve enviar seu resultado.

        // 3. Obter operandos (Vj, Vk) ou tags de dependência (Qj, Qk) para a RS.
        // Operando 1 (Vj/Qj):
        if (originalInst.type == LOAD)
        { // Para LOAD, src1 é o offset, armazenado em 'A'.
            rs->A = atoi(originalInst.src1.c_str());
            rs->Vj = 0;
            rs->Qj = ""; // Vj/Qj não são usados para um operando registrador em LOAD.
        }
        else
        { // Para ADD, SUB, MUL, DIV (src1 é registrador) e STORE (src1 é registrador do dado).
            if (!originalInst.src1.empty())
            { // Verifica se src1 existe (pode não existir para algumas arquiteturas/instruções)
                if (regStatus[originalInst.src1].busy)
                { // Se o valor de src1 está pendente.
                    int producingRobIdx = regStatus[originalInst.src1].robIndex;
                    // Verifica se o valor já está pronto na entrada do ROB que o produzirá.
                    if (rob[producingRobIdx].busy && rob[producingRobIdx].state == ROB_WRITERESULT && rob[producingRobIdx].valueReady)
                    {
                        rs->Vj = rob[producingRobIdx].value; // Pega valor direto do ROB.
                        rs->Qj = "";
                    }
                    else
                    {
                        rs->Qj = to_string(producingRobIdx); // Armazena a tag do ROB que fornecerá o valor.
                    }
                }
                else
                { // Valor de src1 está disponível no banco de registradores.
                    rs->Vj = registers[originalInst.src1];
                    rs->Qj = "";
                }
            }
            else
            {
                rs->Vj = 0;
                rs->Qj = "";
            } // Sem src1
        }

        // Operando 2 (Vk/Qk): src2 para Arith; Registrador Base para Load/Store.
        if (originalInst.type == ADD || originalInst.type == SUB || originalInst.type == MUL || originalInst.type == DIV ||
            originalInst.type == LOAD || originalInst.type == STORE)
        { // Instruções que podem ter src2.
            if (!originalInst.src2.empty())
            { // Verifica se src2 (registrador) existe.
                if (regStatus[originalInst.src2].busy)
                {
                    int producingRobIdx = regStatus[originalInst.src2].robIndex;
                    if (rob[producingRobIdx].busy && rob[producingRobIdx].state == ROB_WRITERESULT && rob[producingRobIdx].valueReady)
                    {
                        rs->Vk = rob[producingRobIdx].value;
                        rs->Qk = "";
                    }
                    else
                    {
                        rs->Qk = to_string(producingRobIdx);
                    }
                }
                else
                {
                    rs->Vk = registers[originalInst.src2];
                    rs->Qk = "";
                }
            }
            else
            {               // Sem src2 explícito (ex: L.D F1, 100() implicaria base 0 ou erro).
                rs->Vk = 0; // Se src2 vazio, assume-se valor 0 para base (ou deveria ser erro de formato).
                rs->Qk = "";
            }
        }
        else
        {
            rs->Vk = 0;
            rs->Qk = "";
        } // Instruções sem src2.

        // Tratamento específico para STORE no momento do Issue:
        if (originalInst.type == STORE)
        {
            rs->A = atoi(originalInst.dest.c_str()); // Para STORE, inst.dest (parseado do offset(base)) é o offset.
            // Se o valor a ser armazenado (Vj, que veio de inst.src1) já está disponível na RS,
            // pode-se já preencher o campo 'value' e 'valueReady' na entrada do ROB do STORE.
            if (rs->Qj.empty())
            {                                      // Se Vj (dado do store) está pronto.
                rob[currentRobIdx].value = rs->Vj; // 'value' na ROB do STORE é o dado a ser escrito.
                rob[currentRobIdx].valueReady = true;
            }
        }

        // 4. Atualizar Tabela de Status do Registrador de Destino (Renomeação).
        // Se a instrução tem um registrador de destino (não é STORE),
        // marca o registrador como 'busy' e aponta para a entrada do ROB que calculará seu novo valor.
        if (originalInst.type != STORE)
        {
            regStatus[originalInst.dest].busy = true;
            regStatus[originalInst.dest].robIndex = currentRobIdx;
        }

        nextInstructionIndex++;
        return true;
    }

    // Inicia a execução de instruções nas RSs cujos operandos (Vj, Vk) estão prontos (Qj, Qk vazios).
    void startExecution()
    {
        ReservationStation *rs_arrays[] = {addRS.data(), mulRS.data(), loadRS.data(), storeRS.data()};
        size_t rs_sizes[] = {addRS.size(), mulRS.size(), loadRS.size(), storeRS.size()};
        string rs_types[] = {"ADD", "MUL", "LOAD", "STORE"};
        int base_latencies[] = {ADD_LATENCY, 0, LOAD_LATENCY, STORE_LATENCY}; // Latência de MUL/DIV é especial.

        for (int type_idx = 0; type_idx < 4; ++type_idx)
        { // Itera sobre os tipos de RS
            for (size_t i = 0; i < rs_sizes[type_idx]; ++i)
            { // Itera sobre cada RS daquele tipo
                ReservationStation &currentRS = rs_arrays[type_idx][i];
                // Verifica se a RS está ocupada e se todos os operandos estão prontos.
                if (currentRS.busy && currentRS.Qj.empty() && currentRS.Qk.empty())
                {
                    bool alreadyExecuting = false;
                    // Verifica se a instrução desta RS já não está na lista de execução.
                    for (const auto &execInst : executingInstructions)
                    {
                        if (execInst.rsType == rs_types[type_idx] && execInst.rsIndex == static_cast<int>(i))
                        {
                            alreadyExecuting = true;
                            break;
                        }
                    }

                    if (!alreadyExecuting)
                    {                                                     // Se não está executando, pode iniciar.
                        int robIdxForInst = stoi(currentRS.destRobIndex); // Pega o índice do ROB associado.
                        // Atualiza o estado da instrução no ROB para EXECUTE, se a entrada do ROB ainda for relevante.
                        if (rob[robIdxForInst].busy && rob[robIdxForInst].state == ROB_ISSUE)
                        {
                            rob[robIdxForInst].state = ROB_EXECUTE;
                        }

                        ExecutingInstruction exec; // Cria uma nova entrada para a lista de instruções em execução.
                        exec.rsIndex = i;
                        exec.rsType = rs_types[type_idx];
                        exec.instructionIndex = currentRS.instructionIndex;

                        // Define a latência correta.
                        if (rs_types[type_idx] == "MUL")
                        { // MUL e DIV usam a mesma RS.
                            exec.remainingCycles = (currentRS.op == MUL) ? MUL_LATENCY : DIV_LATENCY;
                        }
                        else
                        {
                            exec.remainingCycles = base_latencies[type_idx];
                        }
                        executingInstructions.push_back(exec);

                        // Para STORE: se o valor (Vj) se tornou pronto agora (Qj foi resolvido por um WriteBack anterior),
                        // e a instrução está começando a execução (Qk também resolvido),
                        // atualiza a entrada do ROB do STORE com o valor e marca como pronto.
                        if (currentRS.op == STORE && rob[robIdxForInst].busy && !rob[robIdxForInst].valueReady)
                        {
                            rob[robIdxForInst].value = currentRS.Vj; // Vj deve estar pronto aqui.
                            rob[robIdxForInst].valueReady = true;
                        }
                    }
                }
            }
        }
    }

    // Avança a execução das instruções que estão nas unidades funcionais.
    void advanceExecution()
    {
        for (size_t i = 0; i < executingInstructions.size(); /* não incrementa aqui ao apagar */)
        {
            executingInstructions[i].remainingCycles--; // Decrementa ciclos restantes.
            if (executingInstructions[i].remainingCycles <= 0)
            { // Se a execução terminou.
                // Registra o ciclo de conclusão da execução.
                instructions[executingInstructions[i].instructionIndex].execComp = cycle;
                // Adiciona à fila para escrita no CDB/ROB no próximo ciclo.
                completedForCDB.push(executingInstructions[i].instructionIndex);
                // Remove da lista de instruções em execução.
                executingInstructions.erase(executingInstructions.begin() + i);
            }
            else
            {
                i++; // Próxima instrução em execução.
            }
        }
    }

    // Processa resultados do CDB: escreve na entrada do ROB e encaminha para RSs dependentes.
    void processWriteBack()
    {
        if (completedForCDB.empty())
            return; // Nada a fazer se a fila do CDB está vazia.

        int originalInstIndex = completedForCDB.front(); // Pega a próxima instrução da fila.
        completedForCDB.pop();

        Instruction &inst = instructions[originalInstIndex];
        // Registra o ciclo em que o resultado é escrito no ROB (via CDB).
        inst.writeResult = cycle;

        ReservationStation *rs = nullptr;
        string robIndexStr; // String contendo o índice do ROB de destino desta RS.

        // Encontra a RS que originou esta instrução e o índice do ROB associado.
        bool found_rs = false;
        if (!found_rs)
            for (size_t i = 0; i < addRS.size(); ++i)
                if (addRS[i].instructionIndex == originalInstIndex && addRS[i].busy)
                {
                    rs = &addRS[i];
                    robIndexStr = rs->destRobIndex;
                    found_rs = true;
                    break;
                }
        if (!found_rs)
            for (size_t i = 0; i < mulRS.size(); ++i)
                if (mulRS[i].instructionIndex == originalInstIndex && mulRS[i].busy)
                {
                    rs = &mulRS[i];
                    robIndexStr = rs->destRobIndex;
                    found_rs = true;
                    break;
                }
        if (!found_rs)
            for (size_t i = 0; i < loadRS.size(); ++i)
                if (loadRS[i].instructionIndex == originalInstIndex && loadRS[i].busy)
                {
                    rs = &loadRS[i];
                    robIndexStr = rs->destRobIndex;
                    found_rs = true;
                    break;
                }
        if (!found_rs)
            for (size_t i = 0; i < storeRS.size(); ++i)
                if (storeRS[i].instructionIndex == originalInstIndex && storeRS[i].busy)
                {
                    rs = &storeRS[i];
                    robIndexStr = rs->destRobIndex;
                    found_rs = true;
                    break;
                }

        if (rs == nullptr || robIndexStr.empty())
        {
            // Pode acontecer se a instrução já foi cometida ou invalidada (ex: desvio não previsto).
            // Para este simulador, pode ser um sinal de erro ou de uma condição de corrida sutil.
            // cout << "Alerta no WriteBack: RS para inst " << originalInstIndex << " não encontrada ou robIndexStr vazio (possivelmente já cometida)." << endl;
            return;
        }
        int producingRobIdx = stoi(robIndexStr); // Converte a string do índice do ROB para inteiro.

        int resultData = 0;    // Para resultados de ALU e LOAD.
        int effectiveAddr = 0; // Para endereços de LOAD/STORE.

        // Calcula o resultado ou endereço e prepara para atualizar o ROB.
        switch (inst.type)
        {
        case ADD:
            resultData = rs->Vj + rs->Vk;
            break;
        case SUB:
            resultData = rs->Vj - rs->Vk;
            break;
        case MUL:
            resultData = rs->Vj * rs->Vk;
            break;
        case DIV:
            if (rs->Vk != 0)
                resultData = rs->Vj / rs->Vk;
            else
            {
                cerr << "Erro: Divisao por zero na instrucao " << originalInstIndex << "!" << endl;
                resultData = 0;
            }
            break;
        case LOAD:
            effectiveAddr = rs->A + rs->Vk; // A é offset, Vk é valor do registrador base.
            if (effectiveAddr >= 0 && effectiveAddr < 1024)
                resultData = memory[effectiveAddr];
            else
            {
                cerr << "Erro: Endereco de LOAD invalido (" << effectiveAddr << ") para inst " << originalInstIndex << endl;
                resultData = 0;
            } // Tratar erro de acesso.
            rob[producingRobIdx].address = effectiveAddr; // Armazena o endereço calculado no ROB.
            break;
        case STORE:
            effectiveAddr = rs->A + rs->Vk;
            rob[producingRobIdx].address = effectiveAddr; // Armazena o endereço calculado no ROB.
            // O valor a ser armazenado (rs->Vj) já deve estar em rob[producingRobIdx].value
            // e rob[producingRobIdx].valueReady deve ser true, se o dado estava pronto.
            // Se o dado dependia de outra instrução, updateDependentRS terá atualizado o ROB.
            resultData = rs->Vj; // O valor do STORE (Vj) é "transmitido" para sua própria entrada no ROB.
            break;
        case INVALID:
            cerr << "Erro: Instrução inválida no WriteBack para índice " << originalInstIndex << endl;
            resultData = 0;
            return;
        }

        // Atualiza a entrada do ROB com o resultado/valor e estado.
        if (rob[producingRobIdx].busy)
        {                                                 // Verifica se a entrada do ROB ainda é relevante.
            rob[producingRobIdx].value = resultData;      // Armazena o resultado/dado.
            rob[producingRobIdx].valueReady = true;       // Marca que o valor está pronto.
            rob[producingRobIdx].state = ROB_WRITERESULT; // Pronta para commit.

            // Transmite o resultado (valor e ÍNDICE DO ROB que o produziu) para RSs dependentes.
            updateDependentRS(producingRobIdx, resultData);
        }

        // Libera a Estação de Reserva.
        rs->busy = false;
        rs->instructionIndex = -1;
        rs->Qj = "";
        rs->Qk = "";
        rs->Vj = 0;
        rs->Vk = 0;
        rs->A = 0;
        rs->destRobIndex = "";
    }

    // Atualiza Estações de Reserva que aguardam um resultado que foi disponibilizado no CDB (originado de uma entrada do ROB).
    void updateDependentRS(int producingRobIdx, int resultValue)
    {
        string producingTag = to_string(producingRobIdx); // Tag do ROB que produziu o resultado.
        ReservationStation *rs_arrays[] = {addRS.data(), mulRS.data(), loadRS.data(), storeRS.data()};
        size_t rs_sizes[] = {addRS.size(), mulRS.size(), loadRS.size(), storeRS.size()};

        for (int type_idx = 0; type_idx < 4; ++type_idx)
        { // Itera sobre os tipos de RS.
            for (size_t i = 0; i < rs_sizes[type_idx]; ++i)
            { // Itera sobre cada RS.
                ReservationStation &currentRS = rs_arrays[type_idx][i];
                if (currentRS.busy)
                { // Se a RS está ocupada.
                    // Se o operando Qj estava esperando por esta tag do ROB.
                    if (currentRS.Qj == producingTag)
                    {
                        currentRS.Vj = resultValue; // Recebe o valor.
                        currentRS.Qj = "";          // Limpa a tag de espera.
                        // Caso especial: se esta RS é de um STORE e Qj era o operando de DADO,
                        // o valor precisa ser propagado para a entrada do ROB do STORE.
                        if (currentRS.op == STORE)
                        {
                            int storeOwnRobIdx = stoi(currentRS.destRobIndex); // Índice do ROB do próprio STORE.
                            if (rob[storeOwnRobIdx].busy)
                            {                                            // Verifica se a entrada do ROB ainda é relevante.
                                rob[storeOwnRobIdx].value = resultValue; // Atualiza o valor a ser armazenado.
                                rob[storeOwnRobIdx].valueReady = true;   // Marca que o dado está pronto.
                            }
                        }
                    }
                    // Se o operando Qk estava esperando por esta tag do ROB.
                    if (currentRS.Qk == producingTag)
                    {
                        currentRS.Vk = resultValue; // Recebe o valor.
                        currentRS.Qk = "";          // Limpa a tag de espera.
                    }
                }
            }
        }
    }

    // Estágio de Commit: Efetiva o resultado da instrução na cabeça do ROB no estado arquitetural.
    // Garante a terminação em ordem.
    void commitInstruction()
    {
        // Não faz nada se o ROB estiver vazio ou se a entrada na cabeça não estiver ocupada.
        if (robEntriesAvailable == ROB_SIZE || !rob[robHead].busy)
            return;

        ReorderBufferEntry &headEntry = rob[robHead]; // Referência para a entrada na cabeça do ROB.

        // Só pode cometer se o resultado da instrução está pronto no ROB (estado ROB_WRITERESULT).
        if (headEntry.state == ROB_WRITERESULT)
        {
            // Para STOREs, uma condição adicional: o valor a ser armazenado DEVE estar pronto.
            if (headEntry.type == STORE && !headEntry.valueReady)
            {
                // Se um STORE está na cabeça do ROB, mas seu dado ainda não está pronto,
                // ele bloqueia o commit de todas as instruções subsequentes.
                // Este é um ponto importante para a correção de STOREs.
                return;
            }

            Instruction &originalInst = instructions[headEntry.instructionIndex];
            originalInst.commitCycle = cycle; // Registra o ciclo de commit.

            string committedActionLog; // Para a mensagem de log do commit.

            // Efetiva a escrita no estado arquitetural (registradores ou memória).
            if (headEntry.type != STORE)
            { // Para ADD, SUB, MUL, DIV, LOAD
                registers[headEntry.destinationRegister] = headEntry.value;
                committedActionLog = headEntry.destinationRegister + " = " + to_string(headEntry.value);
                // Libera o RegisterStatus se esta entrada do ROB era a última fonte para este registrador.
                if (regStatus[headEntry.destinationRegister].busy &&
                    regStatus[headEntry.destinationRegister].robIndex == robHead)
                {
                    regStatus[headEntry.destinationRegister].busy = false;
                    regStatus[headEntry.destinationRegister].robIndex = -1;
                }
            }
            else
            { // Para STORE
                // Verifica se o endereço é válido antes de escrever na memória.
                if (headEntry.address >= 0 && headEntry.address < 1024)
                {
                    memory[headEntry.address] = headEntry.value;
                    committedActionLog = "MEM[" + to_string(headEntry.address) + "] = " + to_string(headEntry.value);
                }
                else
                {
                    cerr << "Erro CRITICO no Commit: Endereco de STORE invalido: " << headEntry.address << " para inst " << headEntry.instructionIndex << endl;
                    committedActionLog = "STORE ERRO - Endereco Invalido";
                    // O que fazer aqui? Parar simulação? Marcar erro?
                    // Por ora, a instrução será "cometida" (ROB liberado) mas com erro.
                }
            }

            // Log da ação de commit.
            cout << "Ciclo " << cycle << ": Commit Inst " << headEntry.instructionIndex << " (ROB " << robHead << "): " << committedActionLog << endl;

            // Libera a entrada do ROB.
            headEntry.busy = false;
            headEntry.state = ROB_EMPTY;        // Marca como vazia para reutilização.
            robHead = (robHead + 1) % ROB_SIZE; // Avança a cabeça do ROB.
            robEntriesAvailable++;
        }
    }

public:
    // Construtor: inicializa o simulador com contagens de RS e tamanho do ROB.
    TomasuloSimulator(int addRSCount = 3, int mulRSCount = 2, int loadRSCount = 3, int storeRSCount = 3, int rob_s = 16) : ADD_LATENCY(2), MUL_LATENCY(10), DIV_LATENCY(40), LOAD_LATENCY(2), STORE_LATENCY(2), // Inicializa consts
                                                                                                                           ROB_SIZE(rob_s), rob(rob_s)                                                          // Inicializa o ROB com o tamanho passado ou padrão.
    {
        addRS.resize(addRSCount);
        mulRS.resize(mulRSCount);
        loadRS.resize(loadRSCount);
        storeRS.resize(storeRSCount);
        robHead = 0;
        robTail = 0;
        robEntriesAvailable = ROB_SIZE;

        // Inicializa registradores (F0-F31) com valor 10 e status livre.
        for (int i = 0; i < 32; i++)
        {
            string regName = "F" + to_string(i);
            registers[regName] = 10;
            regStatus[regName] = RegisterStatus();
        }
        // Inicializa memória com valores previsíveis (memory[i] = i).
        for (int i = 0; i < 1024; i++)
            memory[i] = i;
        cycle = 0;
        nextInstructionIndex = 0;
    }

    // Carrega instruções de um arquivo.
    bool loadInstructions(const string &filename)
    {
        ifstream file(filename.c_str());
        if (!file.is_open())
        {
            cerr << "Erro ao abrir arquivo: " << filename << endl;
            return false;
        }
        string line;
        while (getline(file, line))
        {
            if (line.empty() || line[0] == '#')
                continue; // Ignora vazias e comentários.
            istringstream iss(line);
            string op, p1, p2, p3; // Tokens temporários para parsing.
            iss >> op >> p1;       // Lê operação e primeiro operando/destino.
            if (!p1.empty() && p1.back() == ',')
                p1.pop_back(); // Remove vírgula.

            Instruction inst; // Cria instrução a ser preenchida.
            // Parseia baseado no tipo de operação.
            if (op == "ADD" || op == "SUB" || op == "MUL" || op == "DIV")
            {
                if (op == "ADD")
                    inst.type = ADD;
                else if (op == "SUB")
                    inst.type = SUB;
                else if (op == "MUL")
                    inst.type = MUL;
                else if (op == "DIV")
                    inst.type = DIV;
                iss >> p2 >> p3;
                if (!p2.empty() && p2.back() == ',')
                    p2.pop_back();
                inst.dest = p1;
                inst.src1 = p2;
                inst.src2 = p3;
            }
            else if (op == "L.D" || op == "LOAD")
            {
                inst.type = LOAD;
                inst.dest = p1; // p1 é registrador destino.
                iss >> p2;      // p2 é a string "offset(base)".
                size_t openParen = p2.find('('), closeParen = p2.find(')');
                if (openParen != string::npos && closeParen != string::npos && closeParen > openParen + 1)
                {
                    inst.src1 = p2.substr(0, openParen);                              // offset.
                    inst.src2 = p2.substr(openParen + 1, closeParen - openParen - 1); // registrador base.
                }
                else
                {
                    cerr << "Formato L.D invalido: " << p2 << " na linha: " << line << endl;
                    continue;
                }
            }
            else if (op == "S.D" || op == "STORE")
            {
                inst.type = STORE;
                inst.src1 = p1; // p1 é registrador do dado.
                iss >> p2;      // p2 é "offset(base)".
                size_t openParen = p2.find('('), closeParen = p2.find(')');
                if (openParen != string::npos && closeParen != string::npos && closeParen > openParen + 1)
                {
                    inst.dest = p2.substr(0, openParen);                              // inst.dest no STORE é usado para o offset.
                    inst.src2 = p2.substr(openParen + 1, closeParen - openParen - 1); // registrador base.
                }
                else
                {
                    cerr << "Formato S.D invalido: " << p2 << " na linha: " << line << endl;
                    continue;
                }
            }
            else
            {
                cerr << "Instrucao nao reconhecida: " << op << " na linha: " << line << endl;
                continue;
            }
            instructions.push_back(inst);
        }
        file.close();
        return true;
    }

    // Verifica se a simulação terminou.
    bool isSimulationComplete() const
    {
        // Se ainda há instruções do programa para serem emitidas.
        if (nextInstructionIndex < static_cast<int>(instructions.size()))
            return false;
        // Se o ROB não está completamente vazio (todas as entradas disponíveis).
        if (robEntriesAvailable != ROB_SIZE)
            return false;
        // Se ainda há instruções em unidades funcionais ou aguardando o CDB.
        if (!executingInstructions.empty() || !completedForCDB.empty())
            return false;

        // Opcional: uma verificação mais rigorosa seria que todas as instruções no vetor 'instructions'
        // tenham um 'commitCycle' válido, mas as condições acima geralmente são suficientes
        // se não houver instruções no início.
        if (!instructions.empty())
        { // Só verifica commitCycle se há instruções.
            for (const auto &inst : instructions)
            {
                if (inst.commitCycle == -1)
                    return false; // Se alguma não foi cometida.
            }
        }
        return true; // Se todas as condições acima indicam conclusão.
    }

    // Avança um ciclo da simulação, executando as fases do pipeline.
    void stepSimulation()
    {
        // A ordem das fases é importante para o fluxo correto de dados e controle.
        // 1. Commit: Libera o ROB, atualiza o estado arquitetural. Permite que o issue avance.
        commitInstruction();
        // 2. WriteResult (CDB->ROB): Resultados das UFs vão para o ROB e são transmitidos via CDB.
        //    Isso libera RSs e disponibiliza operandos para outras instruções.
        processWriteBack();
        // 3. Issue: Novas instruções são alocadas no ROB e nas RSs.
        //    Pode usar recursos/tags do ROB liberados/atualizados pelo Commit e WriteResult.
        issueInstruction();
        // 4. Execute (Start & Advance): Instruções com operandos prontos (potencialmente do WriteResult deste ciclo)
        //    iniciam ou continuam a execução. O estado no ROB é atualizado.
        startExecution();
        advanceExecution();

        cycle++; // Avança o contador de ciclo.
    }

    int getCurrentCycle() const { return cycle; }

    // Imprime os valores finais dos registradores.
    void printRegisters() const
    {
        cout << "\nValores Finais dos Registradores:\n---------------------------------\n";
        for (const auto &pairReg : registers)
        { // Usando range-based for para clareza.
            cout << pairReg.first << " = " << pairReg.second << endl;
        }
        cout << "---------------------------------\n";
    }

    // Imprime o estado detalhado da simulação ciclo a ciclo.
    void printStatus()
    {
        cout << "\n==== Ciclo " << cycle << " ====" << endl;

        // Tabela de Status das Instruções
        cout << "\nInstrucoes:" << endl;
        printf("---------------------------------------------------------------------------------\n");
        printf("| %-1s | %-18s | %-7s | %-9s | %-11s | %-11s |\n", "#", "Instrucao", "Emissao", "Exec Comp", "WriteResult", "Commit");
        printf("---------------------------------------------------------------------------------\n");
        for (size_t i = 0; i < instructions.size(); ++i)
        {
            const Instruction &inst = instructions[i];
            string instStr;
            switch (inst.type)
            {
            case ADD:
                instStr = "ADD " + inst.dest + "," + inst.src1 + "," + inst.src2;
                break;
            case SUB:
                instStr = "SUB " + inst.dest + "," + inst.src1 + "," + inst.src2;
                break;
            case MUL:
                instStr = "MUL " + inst.dest + "," + inst.src1 + "," + inst.src2;
                break;
            case DIV:
                instStr = "DIV " + inst.dest + "," + inst.src1 + "," + inst.src2;
                break;
            case LOAD:
                instStr = "LOAD " + inst.dest + "," + inst.src1 + "(" + inst.src2 + ")";
                break;
            case STORE:
                instStr = "STORE " + inst.src1 + "," + inst.dest + "(" + inst.src2 + ")";
                break;
            default:
                instStr = "INVALID";
                break;
            }
            printf("| %-1zu | %-18s | %-7s | %-9s | %-11s | %-11s |\n", i, instStr.c_str(),
                   (inst.issue != -1 ? to_string(inst.issue).c_str() : "-"),
                   (inst.execComp != -1 ? to_string(inst.execComp).c_str() : "-"),
                   (inst.writeResult != -1 ? to_string(inst.writeResult).c_str() : "-"),
                   (inst.commitCycle != -1 ? to_string(inst.commitCycle).c_str() : "-"));
        }
        printf("---------------------------------------------------------------------------------\n");

        // Tabela de Estações de Reserva (formato unificado)
        const char *rsTableFormat = "| %-1zu | %-4s | %-5s | %-5s | %-5s | %-7s | %-7s | %-10s | %-3s | %-7s |\n";
        const char *rsTableHeader = "| # | Busy | Op    | Vj    | Vk    | Qj(ROB#)| Qk(ROB#)| Dest(ROB#) | A   | InstIdx |\n";
        string rsLineSeparator = "-------------------------------------------------------------------------------------";

        auto printRSGroup = [&](const string &name, const vector<ReservationStation> &rsGroup)
        {
            cout << "\nEstacoes de Reserva " << name << ":" << endl
                 << rsLineSeparator << endl
                 << rsTableHeader << rsLineSeparator << endl;
            for (size_t i = 0; i < rsGroup.size(); ++i)
            {
                const auto &rs = rsGroup[i];
                string opStr;
                if (rs.busy)
                    switch (rs.op)
                    {
                    case ADD:
                        opStr = "ADD";
                        break;
                    case SUB:
                        opStr = "SUB";
                        break;
                    case MUL:
                        opStr = "MUL";
                        break;
                    case DIV:
                        opStr = "DIV";
                        break;
                    case LOAD:
                        opStr = "LOAD";
                        break;
                    case STORE:
                        opStr = "STORE";
                        break;
                    default:
                        opStr = "???";
                    }
                printf(rsTableFormat, i, (rs.busy ? "Sim" : "Nao"), opStr.c_str(),
                       (rs.busy && rs.Qj.empty() ? to_string(rs.Vj).c_str() : "-"),
                       (rs.busy && rs.Qk.empty() ? to_string(rs.Vk).c_str() : "-"),
                       (rs.busy && !rs.Qj.empty() ? rs.Qj.c_str() : "-"),
                       (rs.busy && !rs.Qk.empty() ? rs.Qk.c_str() : "-"),
                       (rs.busy ? rs.destRobIndex.c_str() : "-"),
                       (rs.busy && (rs.op == LOAD || rs.op == STORE) ? to_string(rs.A).c_str() : "-"), // Mostrar A apenas para L/S
                       (rs.busy && rs.instructionIndex != -1 ? to_string(rs.instructionIndex).c_str() : "-"));
            }
            cout << rsLineSeparator << endl;
        };
        printRSGroup("ADD/SUB", addRS);
        printRSGroup("MUL/DIV", mulRS);
        printRSGroup("LOAD", loadRS);
        printRSGroup("STORE", storeRS);

        // Tabela do Reorder Buffer (ROB)
        cout << "\nReorder Buffer (ROB): Head=" << robHead << ", Tail=" << robTail << ", Available=" << robEntriesAvailable << endl;
        const char *robTableFormat = "| %-4d | %-4s | %-7s | %-5s | %-11s | %-7s | %-6s | %-5s | %-7s |\n";
        cout << "------------------------------------------------------------------------------------------" << endl;
        cout << "| ROB# | Busy | InstIdx | Type  | State       | DestReg | ValRdy | Value | Address |" << endl;
        cout << "------------------------------------------------------------------------------------------" << endl;
        for (int i = 0; i < ROB_SIZE; ++i)
        {
            const auto &entry = rob[i];
            string typeStr = entry.busy ? (entry.type == ADD ? "ADD" : entry.type == SUB ? "SUB"
                                                                   : entry.type == MUL   ? "MUL"
                                                                   : entry.type == DIV   ? "DIV"
                                                                   : entry.type == LOAD  ? "LOAD"
                                                                   : entry.type == STORE ? "STORE"
                                                                                         : "INV")
                                        : "---";
            string stateStr = entry.busy ? (entry.state == ROB_ISSUE ? "Issue" : entry.state == ROB_EXECUTE   ? "Execute"
                                                                             : entry.state == ROB_WRITERESULT ? "WriteRes"
                                                                                                              : "Empty")
                                         : "---";
            string value_s = (entry.busy && entry.valueReady) ? to_string(entry.value) : "-";
            string address_s = (entry.busy && (entry.type == LOAD || entry.type == STORE) && entry.address != 0) ? to_string(entry.address) : "-"; // Mostrar endereço se for L/S e calculado.
            if (entry.busy && entry.type == STORE && !entry.valueReady && entry.state == ROB_WRITERESULT)
                value_s = "(pend)"; // Dado do store pendente mas endereço pronto

            printf(robTableFormat, i, (entry.busy ? "Sim" : "Nao"),
                   (entry.busy && entry.instructionIndex != -1 ? to_string(entry.instructionIndex).c_str() : "-"),
                   typeStr.c_str(), stateStr.c_str(),
                   (entry.busy ? entry.destinationRegister.c_str() : "-"),
                   (entry.busy ? (entry.valueReady ? "Sim" : "Nao") : "-"),
                   value_s.c_str(), address_s.c_str());
        }
        cout << "------------------------------------------------------------------------------------------" << endl;

        // Tabela de Status dos Registradores
        cout << "\nRegister Status:" << endl;
        printf("---------------------\n");
        printf("| Reg | Busy | ROB# |\n");
        printf("---------------------\n");
        bool anyRegBusy = false;
        for (const auto &pair : regStatus)
        {
            if (pair.second.busy)
            {
                anyRegBusy = true;
                printf("| %-3s | %-4s | %-4s |\n",
                       pair.first.c_str(), "Sim", to_string(pair.second.robIndex).c_str());
            }
        }
        if (!anyRegBusy)
        {
            printf("| --- | Nao  | -    |\n"); // Linha para indicar que todos estão livres
        }
        printf("---------------------\n");
    }
}; // Fim da classe TomasuloSimulator

// Função principal - Ponto de entrada do programa
int main()
{
    // Cria o simulador com contagens específicas de RS e tamanho do ROB.
    // Ex: TomasuloSimulator simulator(3, 2, 3, 3, 8); // 3 AddRS, 2 MulRS, 3 LoadRS, 3 StoreRS, ROB com 8 entradas.
    TomasuloSimulator simulator; // Usa tamanhos padrão (3,2,3,3 para RSs, 16 para ROB).

    string filename;
    cout << "Digite o nome do arquivo de instrucoes: ";
    cin >> filename; // Solicita o nome do arquivo de entrada.

    if (!simulator.loadInstructions(filename))
    { // Carrega as instruções.
        cerr << "Falha ao carregar instrucoes. Finalizando." << endl;
        return 1;
    }

    // Loop principal da simulação: continua até todas as instruções serem cometidas.
    while (!simulator.isSimulationComplete())
    {
        simulator.printStatus();    // Imprime o estado atual.
        simulator.stepSimulation(); // Avança um ciclo.

        cout << "\nAvancar para o proximo ciclo? [Pressione ENTER]";
        // Limpa o buffer de entrada para garantir que o cin.get() funcione corretamente após um cin >> string.
        if (cin.peek() == '\n')
            cin.ignore();
        else
            cin.ignore(numeric_limits<streamsize>::max(), '\n');
        cin.get(); // Aguarda o usuário.
    }

    // Simulação concluída.
    cout << "\n=== Simulacao concluida no ciclo " << simulator.getCurrentCycle() - 1 << " ===" << endl;
    simulator.printStatus();    // Imprime o estado final.
    simulator.printRegisters(); // Imprime os valores finais dos registradores.
    return 0;
}